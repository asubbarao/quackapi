"""Verify HTTP keep-alive clients do not abort during process shutdown.

Usage: python test/python/test_http_pool_shutdown.py QUACKAPI_EXTENSION
"""

import os
import subprocess
import sys
import time
from pathlib import Path


def connection_config():
    config = {"allow_unsigned_extensions": "true"}
    extension_directory = os.environ.get("DUCKDB_EXTENSION_DIRECTORY")
    if extension_directory:
        config["extension_directory"] = extension_directory
    return config


def run_requests(extension):
    import duckdb

    extension = str(Path(extension).resolve()).replace("'", "''")
    for cycle in range(20):
        connection = duckdb.connect(config=connection_config())
        try:
            connection.execute("LOAD httpfs")
            connection.execute(f"LOAD '{extension}'")
            status, error = connection.execute(
                "SELECT status, error FROM quackapi_post(" "'https://httpbin.org/post', '{\"probe\":\"quackapi-65\"}')"
            ).fetchone()
            if status != 200 or error is not None:
                raise RuntimeError(f"POST failed: status={status}, error={error}")
        finally:
            connection.close()
        if cycle < 19:
            time.sleep(15)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--child":
        run_requests(sys.argv[2])
        return
    if len(sys.argv) != 2:
        raise SystemExit("Usage: test_http_pool_shutdown.py QUACKAPI_EXTENSION")

    result = subprocess.run(
        [sys.executable, __file__, "--child", sys.argv[1]],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"HTTP pool child exited {result.returncode}\n" f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


if __name__ == "__main__":
    main()
