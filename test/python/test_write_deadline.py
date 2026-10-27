"""Exercise socket backpressure without shipping a stalled GET SQL function.

Usage: python test/python/test_write_deadline.py DUCKDB_BINARY QUACKAPI_EXTENSION
"""

import socket
import subprocess
import sys
import time
from pathlib import Path


def receive_after_stall(port, path):
    with socket.socket() as client:
        client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        client.settimeout(35)
        client.connect(("127.0.0.1", port))
        client.sendall(f"GET /{path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n".encode())
        time.sleep(6)
        received = bytearray()
        while True:
            try:
                chunk = client.recv(65536)
            except ConnectionResetError:
                break
            if not chunk:
                break
            received.extend(chunk)
        return bytes(received).partition(b"\r\n\r\n")[2]


def main():
    binary, extension = sys.argv[1:]
    # Reserve an ephemeral port, then release it immediately before startup.
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    extension = str(Path(extension).resolve()).replace("'", "''")
    sql = f"""
LOAD '{extension}';
CREATE ROUTE control GET '/control' AS SELECT repeat('x', 10000000) AS payload;
CREATE ROUTE treatment GET '/treatment' WITH (timeout_sec := 30)
AS SELECT repeat('x', 10000000) AS payload;
SELECT * FROM quackapi_serve({port}, host := '127.0.0.1', access_log := false,
 http_client := 'httplib', write_timeout_sec := 2, compression := false, block := true);
"""
    with subprocess.Popen(
        [binary, "-unsigned", "-c", sql], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
    ) as server:
        try:
            deadline = time.monotonic() + 20
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                        break
                except OSError:
                    if server.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("Server did not become ready")
                    time.sleep(0.05)
            control = receive_after_stall(port, "control")
            treatment = receive_after_stall(port, "treatment")
            assert len(control) < 10000014, len(control)
            assert len(treatment) >= 10000014, len(treatment)
            print("Socket backpressure: control truncated; route override completed")
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


if __name__ == "__main__":
    main()
