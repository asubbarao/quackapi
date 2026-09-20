#!/usr/bin/env python3
"""Validate the benchmark configuration, and prove the bench still fails.

The bench records its failures as JSON. What rots silently is the path from a
violated contract to a nonzero exit, so every bench program is run twice here
against a stub server -- once honest, once mutated -- and the exit codes are
asserted. A gate that stops firing is a CI failure instead of a green run.

Needs nothing but the standard library: no built extension, no venv, no
network, no k6.
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
        if Stub.mode == "wrong_body":
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
        "the pinned application root is the one run.sh serves",
        manifest["application_root"].endswith("bigger_applications/app_an_py310"),
        manifest["application_root"],
    )

    lines = [line.strip() for line in (BENCH_DIR / "requirements.txt").read_text().splitlines()]
    floating = [line for line in lines if line and not line.startswith("#") and "==" not in line]
    holds("every benchmark dependency is pinned", not floating, f"floating: {floating}")

    llm = (BENCH_DIR / "scenarios" / "llm.js").read_text()
    fanout = (BENCH_DIR / "scenarios" / "fanout.js").read_text()
    # k6 cannot be run here, so these are source guards on the exact defect:
    # checks with no threshold let k6 exit 0 with every check failed.
    for scenario, text in (("llm.js", llm), ("fanout.js", fanout)):
        holds(f"{scenario} ties its exit code to its checks", "checks: ['rate==1']" in text, "no checks threshold")
        holds(f"{scenario} declares no empty threshold set", "thresholds: {}" not in text, "thresholds: {} is back")
    holds("llm.js gates logical success", "logical_success: ['rate==1']" in llm, "no logical_success threshold")
    holds(
        "llm.js checks the status the route contracts to return",
        "res.status === 200" in llm and "res.status < 300" not in llm,
        "the any-2xx status check is back",
    )

    run_sh = (BENCH_DIR / "run.sh").read_text()
    for label in ('gate "conformance:', 'gate "loadgen:', 'gate "crash:'):
        holds(f"run.sh runs {label.split(chr(34))[1][:-1]} through the gate", label in run_sh, f"{label} missing")
    holds(
        "run.sh turns recorded gates into a nonzero exit",
        'if [[ -n "${GATE_FAILURES}" ]]; then' in run_sh,
        "the final verdict is missing",
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
