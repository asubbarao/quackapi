#!/usr/bin/env python3
"""Run SQLLogic and reject incomplete or under-counted product suites."""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXPECTED_FILES = 36
BASE_ASSERTIONS = 3089
OPTIONAL_ASSERTIONS = {
    "QUACKAPI_TEST_JSON_SCHEMA": 14,
    "QUACKAPI_TEST_PG_DSN": 13,
}
SENTINEL = "test/sql/zz_build_guard.test"


def main() -> int:
    runner = Path(sys.argv[1] if len(sys.argv) > 1 else "build/release/test/unittest")
    if not runner.is_absolute():
        runner = ROOT / runner

    source_tests = sorted(str(path.relative_to(ROOT)) for path in (ROOT / "test/sql").glob("*.test"))
    if len(source_tests) != EXPECTED_FILES or source_tests[-1] != SENTINEL:
        print(
            f"SQLLogic guard: expected {EXPECTED_FILES} files ending in {SENTINEL}; "
            f"found {len(source_tests)} ending in {source_tests[-1] if source_tests else '<none>'}",
            file=sys.stderr,
        )
        return 2

    env = os.environ.copy()
    test_home = ROOT / "build/test-home"
    test_home.mkdir(parents=True, exist_ok=True)
    env["HOME"] = str(test_home)
    env["USERPROFILE"] = str(test_home)

    common = [str(runner), "--test-config", "test/sql_suite.json", "test/*", "--order", "lex"]
    listed = subprocess.run(
        common + ["--list-test-names-only"], cwd=ROOT, env=env, text=True, capture_output=True, check=False
    )
    registered = sorted(line.strip() for line in listed.stdout.splitlines() if line.strip().endswith(".test"))
    # Catch2 returns the number of listed cases, capped at 255.
    if listed.returncode != min(len(registered), 255) or registered != source_tests:
        print("SQLLogic guard: registered cases do not match test/sql/*.test", file=sys.stderr)
        print(listed.stdout, end="", file=sys.stderr)
        print(listed.stderr, end="", file=sys.stderr)
        return 2

    proc = subprocess.Popen(common, cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert proc.stdout is not None
    output_lines = []
    for line in proc.stdout:
        print(line, end="")
        output_lines.append(line)
    returncode = proc.wait()
    output = "".join(output_lines)
    if returncode != 0:
        return returncode

    summary = re.search(
        r"All tests passed \((?:(\d+) skipped tests?, )?(\d+) assertions in (\d+) test cases\)", output
    )
    if not summary:
        print("SQLLogic guard: passing summary was not found", file=sys.stderr)
        return 2

    skipped = int(summary.group(1) or 0)
    assertions = int(summary.group(2))
    cases = int(summary.group(3))
    missing_optional = sum(name not in env for name in OPTIONAL_ASSERTIONS)
    expected_assertions = BASE_ASSERTIONS + sum(
        extra for name, extra in OPTIONAL_ASSERTIONS.items() if name in env
    )
    expected_cases = EXPECTED_FILES - missing_optional

    if SENTINEL not in output or skipped != missing_optional or assertions != expected_assertions or cases != expected_cases:
        print(
            "SQLLogic guard: incomplete suite: "
            f"sentinel={SENTINEL in output}, skipped={skipped}/{missing_optional}, "
            f"assertions={assertions}/{expected_assertions}, cases={cases}/{expected_cases}",
            file=sys.stderr,
        )
        return 2

    print(f"SQLLogic guard: complete ({assertions} assertions across {cases} runnable cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
