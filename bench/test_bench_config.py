#!/usr/bin/env python3
"""Validate the benchmark configuration, and prove the bench still fails.

The bench records its failures as JSON. What rots silently is the path from a
violated contract to a nonzero exit, so every bench program is run twice here
against a stub server -- once honest, once mutated -- and the exit codes are
asserted. A gate that stops firing is a CI failure instead of a green run.

Needs nothing but the standard library: no built extension, no venv, no
network, and no load generator beyond loadgen.py itself.
"""
from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

from conformance import CASES

BENCH_DIR = Path(__file__).resolve().parent
HEX = "0123456789abcdef"
JOBS_PATH = "/bench/jobs?token=jessica"

# The honest stub IS the conformance table, so the fixture cannot drift away
# from the contract the bench asserts. A case with no expected body is one the
# bench only requires to carry a "detail".
HONEST = {
    (method, path, headers.get("X-Token")): (status, body if body is not None else {"detail": "stub validation error"})
    for _name, method, path, headers, status, body in CASES
}


class Stub(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    mode = "honest"

    def log_message(self, *_args) -> None:
        return

    def _respond(self) -> None:
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)
        if Stub.mode == "llm_ok":
            status, body = 200, [{"dims": 768, "response": "hi", "ollama_total_ms": 120.0, "out_tokens": 16}]
        elif Stub.mode == "llm_broken":
            status, body = 200, [{"dims": 0, "response": None, "ollama_total_ms": 0}]
        elif Stub.mode == "wrong_body":
            status, body = 200, {"detail": "constant wrong response"}
        elif Stub.mode == "shed":
            status, body = 503, {"detail": "overloaded"}
        elif Stub.mode == "refuse":
            status, body = 500, {"detail": "stub refuses every job"}
        elif self.command == "POST" and self.path == JOBS_PATH:
            status, body = 202, {"id": 1, "job_id": "stub-job"}
        else:
            status, body = HONEST.get(
                (self.command, self.path, self.headers.get("X-Token")),
                (404, {"detail": "no stub case for this request"}),
            )
        payload = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    do_GET = _respond
    do_POST = _respond
    do_PUT = _respond


FAILURES: list[str] = []


def record(name: str, good: bool, detail: str) -> None:
    print(f"{'PASS' if good else 'FAIL'}: {name}{'' if good else f' -- {detail}'}")
    if not good:
        FAILURES.append(name)


def holds(name: str, condition: bool, detail: str) -> None:
    record(name, condition, detail)


def exits(name: str, mode: str, want_zero: bool, *argv: str) -> None:
    Stub.mode = mode
    completed = subprocess.run([sys.executable, *argv], capture_output=True, text=True)
    code = completed.returncode
    record(name, (code == 0) == want_zero, f"exit {code}, wanted {'0' if want_zero else 'nonzero'}")


def check_configuration() -> None:
    names = [case[0] for case in CASES]
    holds("conformance cases are distinctly named", len(set(names)) == len(names), f"names: {names}")
    requests = [(case[1], case[2], case[3].get("X-Token")) for case in CASES]
    holds(
        "no two conformance cases are the same request with different expectations",
        len(set(requests)) == len(requests),
        f"requests: {requests}",
    )

    manifest = json.loads((BENCH_DIR / "upstream_files.json").read_text())
    commit = manifest["commit"]
    holds(
        "the upstream application is pinned to a full commit",
        len(commit) == 40 and all(character in HEX for character in commit),
        f"commit: {commit}",
    )
    files = manifest["files"]
    unpinned = [name for name, digest in files.items() if len(digest) != 64 or not all(c in HEX for c in digest)]
    holds("every upstream file carries a sha256", bool(files) and not unpinned, f"unpinned: {unpinned}")
    holds(
        "the pinned application root is the one run.sql serves",
        manifest["application_root"].endswith("bigger_applications/app_an_py310"),
        manifest["application_root"],
    )

    lines = [line.strip() for line in (BENCH_DIR / "requirements.txt").read_text().splitlines()]
    floating = [line for line in lines if line and not line.startswith("#") and "==" not in line]
    holds("every benchmark dependency is pinned", not floating, f"floating: {floating}")

    run_sql = (BENCH_DIR / "run.sql").read_text()
    # Every program the bench runs has to report an exit code as a gate row;
    # a program invoked without one would fail silently and the run would pass.
    for label in ("conformance:%s|%s", "loadgen:%s:trial%s:c%s|%s", "crash_client:%s|%s", "crash:%s|%s", "report|%s"):
        holds(f"run.sql records {label.split('|')[0]} as a gate row", label in run_sql, f"{label} missing")
    holds(
        "run.sql turns a recorded gate into a raised error",
        "error('FAIL: the run violated its own contract: '" in run_sql,
        "the final verdict is missing",
    )
    holds(
        "the benchmark ships no shell artifact",
        not list(BENCH_DIR.rglob("*.sh")),
        f"shell artifacts: {[str(path) for path in BENCH_DIR.rglob('*.sh')]}",
    )


def check_gates(port: int, scratch: Path) -> None:
    conformance = [str(BENCH_DIR / "conformance.py"), "--stack", "stub", "--port", str(port)]
    exits("conformance.py accepts an honest stack", "honest", True, *conformance)
    exits("conformance.py rejects a constant wrong response", "wrong_body", False, *conformance)

    loadgen = [
        str(BENCH_DIR / "loadgen.py"), "--url", f"http://127.0.0.1:{port}/users/rick?token=jessica",
        "--stack", "stub", "--trial", "1", "--concurrency", "2", "--duration", "0.3",
        "--warmup", "0", "--timeout", "5", "--expect-json", '{"username":"rick"}',
        "--raw", str(scratch / "raw.csv.gz"),
    ]
    exits("loadgen.py accepts the contracted body", "honest", True, *loadgen)
    exits("loadgen.py rejects a constant wrong response", "wrong_body", False, *loadgen)
    # 503 is the documented overload answer, so this is the zero-success gate
    # firing on its own rather than the contract gate firing again.
    exits("loadgen.py rejects a run with no successful request", "shed", False, *loadgen)

    # The checks that replaced the deleted load-generator scenarios, proven the
    # same way as every other gate: honest answer passes, mutant answer fails.
    def llm(check: str) -> list[str]:
        return [
            str(BENCH_DIR / "loadgen.py"), "--url", f"http://127.0.0.1:{port}/llm/probe?model=m&prompt=p",
            "--method", "POST", "--check", check,
            "--stack", "stub", "--trial", "1", "--concurrency", "2", "--duration", "0.3",
            "--warmup", "0", "--timeout", "5", "--raw", str(scratch / f"{check}.csv.gz"),
        ]

    exits("loadgen.py accepts an embedding with positive dims", "llm_ok", True, *llm("embedding"))
    exits("loadgen.py rejects an embedding with no dimensions", "llm_broken", False, *llm("embedding"))
    exits("loadgen.py accepts a generation with text and upstream timing", "llm_ok", True, *llm("generation"))
    exits("loadgen.py rejects a generation with no text and no timing", "llm_broken", False, *llm("generation"))

    crash_client = [str(BENCH_DIR / "crash_client.py"), "--port", str(port), "--jobs", "4", "--delay-ms", "0"]
    exits("crash_client.py accepts acknowledged jobs", "honest", True, *crash_client)
    exits("crash_client.py rejects a run with nothing acknowledged", "refuse", False, *crash_client)

    client = json.dumps({"attempted": 4, "acknowledged": 4, "acknowledged_ids": [1, 2, 3, 4], "failures": []})
    crash_check = [str(BENCH_DIR / "crash_check.py"), "--attempted", "4", "--acknowledged", "4", "--client-json", client]
    exits("crash_check.py accepts a durable stack that kept every job", "honest", True,
          *crash_check, "--stack", "quackapi", "--durability", "durable", "--survived", "4")
    exits("crash_check.py rejects a durable stack that lost one", "honest", False,
          *crash_check, "--stack", "quackapi", "--durability", "durable", "--survived", "3")
    exits("crash_check.py records the volatile control without failing it", "honest", True,
          *crash_check, "--stack", "fastapi", "--durability", "volatile", "--survived", "0")


def main() -> None:
    server = ThreadingHTTPServer(("127.0.0.1", 0), Stub)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        check_configuration()
        with tempfile.TemporaryDirectory() as scratch:
            check_gates(server.server_address[1], Path(scratch))
    finally:
        server.shutdown()
        server.server_close()
    if FAILURES:
        print("FAIL: " + "; ".join(FAILURES), file=sys.stderr)
        raise SystemExit(1)
    print("PASS: bench configuration is valid and every bench gate still fails when it should")


if __name__ == "__main__":
    main()
