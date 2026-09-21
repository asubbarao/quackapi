"""Smoke the pinned loadable artifact through an isolated loopback listener."""

import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import time
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", required=True)
    parser.add_argument("--extension", required=True, type=Path)
    parser.add_argument("--httpfs-timeout-retry", required=True, type=Path)
    args = parser.parse_args()
    extension = args.extension.resolve()
    companion = args.httpfs_timeout_retry.resolve()
    load_sql = "; ".join(
        "LOAD '" + str(path).replace("'", "''") + "'"
        for path in (companion, extension)
    )
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    sql = f"""
SET memory_limit = '20GiB';
SET threads = 12;
SET max_temp_directory_size = '128GiB';
SET enable_logging = false;
CREATE SCHEMA workspace;
CREATE TABLE workspace.marker AS SELECT 'system-quack-smoke' AS marker;
CREATE ROUTE runtime GET '/runtime' AS
SELECT version() AS version,
       current_setting('memory_limit') AS memory_limit,
       current_setting('threads') AS threads,
       current_setting('preserve_insertion_order') AS preserve_insertion_order,
       current_setting('enable_logging') AS enable_logging,
       (SELECT marker FROM workspace.marker) AS marker,
       (SELECT loaded FROM duckdb_extensions() WHERE extension_name = 'otlp') AS otlp_loaded,
       worker_threads, max_pending_requests
FROM quackapi_servers();
CREATE ROUTE capped GET '/capped' AS SELECT repeat('x', 9000000) AS text;
SELECT * FROM quackapi_serve({port}, host := '127.0.0.1',
    worker_threads := 8, max_pending_requests := 32,
    query_timeout_ms := 30000, max_response_bytes := 8388608,
    tune := false, wire_quack_auth := false, access_log := false,
    block := true);
"""
    process = subprocess.Popen(
        [args.duckdb, "-init", "/dev/null", "-unsigned", ":memory:", "-bail", "-json",
         "-cmd", load_sql, "-c", sql],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    base = f"http://127.0.0.1:{port}"

    def fetch(path):
        try:
            with urllib.request.urlopen(base + path, timeout=5) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    try:
        deadline = time.monotonic() + 20
        while True:
            if process.poll() is not None:
                stdout, stderr = process.communicate()
                raise AssertionError(f"DuckDB exited {process.returncode}: {stdout}\n{stderr}")
            try:
                health_status, health = fetch("/health")
                break
            except urllib.error.URLError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)
        assert health_status == 200, (health_status, health)
        status, runtime = fetch("/runtime")
        expected = [{
            "version": "v1.5.5", "memory_limit": "20.0 GiB", "threads": 12,
            "preserve_insertion_order": True, "enable_logging": False,
            "marker": "system-quack-smoke", "worker_threads": 8,
            "max_pending_requests": 32, "otlp_loaded": False,
        }]
        assert status == 200 and runtime == expected, (status, runtime)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            replies = list(pool.map(lambda _: fetch("/runtime"), range(16)))
        assert all(reply == (200, expected) for reply in replies), replies
        cap_status, cap_body = fetch("/capped")
        assert cap_status == 507, (cap_status, cap_body)
        assert fetch("/runtime") == (200, expected)
        print(json.dumps({
            "engine": args.duckdb, "extension": str(extension),
            "extension_sha256": hashlib.sha256(extension.read_bytes()).hexdigest(),
            "pid": process.pid, "health": health, "runtime": runtime,
            "concurrent_requests": len(replies), "cap_status": cap_status,
            "cap_body": cap_body, "recovered_after_cap": True,
        }, indent=2))
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=5)


if __name__ == "__main__":
    main()
