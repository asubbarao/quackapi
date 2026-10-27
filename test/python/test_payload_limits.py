"""Exercise serve-wide and route-specific HTTP request body limits.

Usage: python test/python/test_payload_limits.py DUCKDB_BINARY QUACKAPI_EXTENSION
"""

import gzip
import socket
import subprocess
import sys
import time
from pathlib import Path


def wait_for_server(process, port):
    deadline = time.monotonic() + 20
    while True:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            if process.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("Server did not become ready")
            time.sleep(0.05)


def json_body(size):
    prefix, suffix = b'{"blob":"', b'"}'
    return prefix + b"x" * (size - len(prefix) - len(suffix)) + suffix


def request(
    port,
    path,
    body,
    chunked=False,
    header_only=False,
    content_encoding=None,
    content_length=None,
):
    with socket.create_connection(("127.0.0.1", port), timeout=15) as client:
        client.settimeout(30)
        headers = [
            f"POST {path} HTTP/1.1",
            "Host: localhost",
            "Content-Type: application/json",
            "Connection: close",
        ]
        if chunked:
            headers.append("Transfer-Encoding: chunked")
        else:
            length = len(body) if content_length is None else content_length
            headers.append(f"Content-Length: {length}")
        if content_encoding:
            headers.append(f"Content-Encoding: {content_encoding}")
        client.sendall(("\r\n".join(headers) + "\r\n\r\n").encode())
        try:
            if header_only:
                pass
            elif chunked:
                for offset in range(0, len(body), 65536):
                    chunk = body[offset : offset + 65536]
                    client.sendall(f"{len(chunk):X}\r\n".encode() + chunk + b"\r\n")
                client.sendall(b"0\r\n\r\n")
            else:
                client.sendall(body)
        except (BrokenPipeError, ConnectionResetError):
            # A bounded server may respond 413 and close while a rejected
            # transfer is still being sent. Read that response below.
            pass
        response = bytearray()
        while True:
            try:
                chunk = client.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            response.extend(chunk)
        return bytes(response)


def status(response):
    return int(response.split(b" ", 2)[1])


def main():
    binary, extension = sys.argv[1:]
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    extension = str(Path(extension).resolve()).replace("'", "''")
    sql = f"""
LOAD '{extension}';
CREATE ROUTE default_body POST '/default' AS SELECT length($body::VARCHAR) AS n;
CREATE ROUTE opt_in POST '/opt-in' WITH (max_request_bytes := 12582912)
AS SELECT length($body::VARCHAR) AS n;
CREATE ROUTE small POST '/small' WITH (max_request_bytes := 1024)
AS SELECT length($body::VARCHAR) AS n;
CREATE ROUTE large_route POST '/large-route' WITH (max_request_bytes := 33554432)
AS SELECT length($body::VARCHAR) AS n;
SELECT * FROM quackapi_serve({port}, host := '127.0.0.1', access_log := false,
 http_client := 'httplib', max_request_bytes := 16777216, block := true);
"""
    with subprocess.Popen(
        [binary, "-unsigned", "-c", sql], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
    ) as server:
        try:
            wait_for_server(server, port)
            large = json_body(9 * 1024 * 1024)
            assert status(request(port, "/default", large, header_only=True)) == 413
            assert status(request(port, "/small", json_body(2048), header_only=True)) == 413
            assert status(request(port, "/small", json_body(2048), chunked=True)) == 413
            # Builds without httplib's request gzip support reject the encoding
            # outright (415); enabled builds enforce the decoded size (413).
            gzip_status = status(request(port, "/small", gzip.compress(json_body(4096)), content_encoding="gzip"))
            assert gzip_status in (413, 415), gzip_status
            assert status(request(port, "/opt-in", large)) == 200
            assert status(request(port, "/opt-in", large, chunked=True)) == 200
            ceiling_headers = request(port, "/large-route", b"", header_only=True, content_length=17 * 1024 * 1024)
            assert status(ceiling_headers) == 413
            print("Request body limits: default, route, serve ceiling, chunked, and gzip limits passed")
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


if __name__ == "__main__":
    main()
