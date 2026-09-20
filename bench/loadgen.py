#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
from collections import Counter
import csv
import gzip
import json
import math
from pathlib import Path
import sys
import time
from urllib.parse import urlsplit


async def read_response(reader: asyncio.StreamReader) -> tuple[int, bytes, bool]:
    status_line = await reader.readline()
    if not status_line:
        raise EOFError("socket closed before an HTTP response")
    parts = status_line.decode("latin1").rstrip().split(" ", 2)
    if len(parts) < 2:
        raise ValueError(f"invalid HTTP status line: {status_line!r}")
    status = int(parts[1])
    headers: dict[str, str] = {}
    while True:
        line = await reader.readline()
        if line in (b"\r\n", b"\n", b""):
            break
        name, value = line.decode("latin1").split(":", 1)
        headers[name.lower()] = value.strip()
    if headers.get("transfer-encoding", "").lower() == "chunked":
        chunks: list[bytes] = []
        while True:
            size = int((await reader.readline()).split(b";", 1)[0], 16)
            if size == 0:
                await reader.readline()
                break
            chunks.append(await reader.readexactly(size))
            await reader.readexactly(2)
        body = b"".join(chunks)
    else:
        length = int(headers.get("content-length", "0"))
        body = await reader.readexactly(length) if length else b""
    return status, body, headers.get("connection", "").lower() == "close"


# Overload is answered with 503 rather than a dropped socket (quackapi_server.cpp),
# so a shed response is a load measurement. Every other departure from the
# contracted 200 body is a correctness failure and fails the run.
SHED_STATUS = 503


def percentile(values: list[float], q: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, math.ceil(q * len(ordered)) - 1)]


async def phase(host: str, port: int, target: str, concurrency: int, duration: float, timeout: float, expected: object) -> list[dict]:
    deadline = time.perf_counter() + duration
    attempts: list[dict] = []

    async def worker() -> None:
        reader: asyncio.StreamReader | None = None
        writer: asyncio.StreamWriter | None = None
        while time.perf_counter() < deadline:
            started = time.perf_counter_ns()
            try:
                if writer is None:
                    reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout)
                request = (
                    f"GET {target} HTTP/1.1\r\nHost: {host}:{port}\r\n"
                    "Accept: application/json\r\nConnection: keep-alive\r\n\r\n"
                ).encode()
                writer.write(request)
                await asyncio.wait_for(writer.drain(), timeout)
                assert reader is not None
                status, body, close = await asyncio.wait_for(read_response(reader), timeout)
                elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
                contract_ok = False
                try:
                    contract_ok = status == 200 and json.loads(body) == expected
                except (json.JSONDecodeError, UnicodeDecodeError):
                    pass
                attempts.append({"latency_ms": elapsed_ms, "status": status, "ok": contract_ok, "error": "" if contract_ok else ("shed" if status == SHED_STATUS else "contract")})
                if close:
                    writer.close()
                    await writer.wait_closed()
                    reader = writer = None
            except Exception as exc:
                elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
                name = type(exc).__name__
                if isinstance(exc, ConnectionResetError):
                    name = "reset"
                elif isinstance(exc, (TimeoutError, asyncio.TimeoutError)):
                    name = "timeout"
                elif isinstance(exc, EOFError):
                    name = "eof"
                attempts.append({"latency_ms": elapsed_ms, "status": None, "ok": False, "error": name})
                if writer is not None:
                    writer.close()
                    try:
                        await writer.wait_closed()
                    except Exception:
                        pass
                reader = writer = None
        if writer is not None:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass

    await asyncio.gather(*(worker() for _ in range(concurrency)))
    return attempts


async def run(args: argparse.Namespace) -> dict:
    parsed = urlsplit(args.url)
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or 80
    target = parsed.path or "/"
    if parsed.query:
        target += "?" + parsed.query
    expected = json.loads(args.expect_json)
    if args.warmup > 0:
        await phase(host, port, target, args.concurrency, args.warmup, args.timeout, expected)
    started = time.perf_counter()
    attempts = await phase(host, port, target, args.concurrency, args.duration, args.timeout, expected)
    elapsed = time.perf_counter() - started
    successful = [row["latency_ms"] for row in attempts if row["ok"]]
    response_latencies = [row["latency_ms"] for row in attempts if row["status"] is not None]
    errors = Counter(row["error"] for row in attempts if row["error"])
    statuses = Counter(str(row["status"]) for row in attempts if row["status"] is not None)
    args.raw.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(args.raw, "wt", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=["latency_ms", "status", "ok", "error"])
        writer.writeheader()
        writer.writerows(attempts)
    contract_failures = errors.pop("contract", 0)
    sheds = errors.pop("shed", 0)
    timeouts = errors.pop("timeout", 0)
    resets = errors.pop("reset", 0)
    eofs = errors.pop("eof", 0)
    return {
        "stack": args.stack, "trial": args.trial, "concurrency": args.concurrency,
        "configured_duration_sec": args.duration, "elapsed_sec": elapsed,
        "attempted": len(attempts), "successful": len(successful),
        "successful_rps": len(successful) / elapsed if elapsed else 0,
        "p50_ms": percentile(successful, 0.50), "p99_ms": percentile(successful, 0.99),
        "max_ms": max(successful) if successful else None,
        "all_response_max_ms": max(response_latencies) if response_latencies else None,
        "http_statuses": dict(statuses), "contract_failures": contract_failures, "shed": sheds,
        "timeouts": timeouts, "resets": resets, "eofs": eofs,
        "other_no_response": sum(errors.values()), "other_errors": dict(errors), "raw": str(args.raw),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--stack", required=True)
    parser.add_argument("--trial", type=int, required=True)
    parser.add_argument("--concurrency", type=int, required=True)
    parser.add_argument("--duration", type=float, required=True)
    parser.add_argument("--warmup", type=float, default=0.5)
    parser.add_argument("--timeout", type=float, default=12)
    parser.add_argument("--expect-json", required=True)
    parser.add_argument("--raw", type=Path, required=True)
    args = parser.parse_args()
    summary = asyncio.run(run(args))
    print(json.dumps(summary, separators=(",", ":")))
    reasons = []
    if summary["contract_failures"]:
        reasons.append(f"{summary['contract_failures']} responses were not the contracted 200 body")
    if not summary["successful"]:
        reasons.append(f"no request of {summary['attempted']} completed the contract")
    if reasons:
        print(f"FAIL: {args.stack} c{args.concurrency} trial{args.trial}: {'; '.join(reasons)}", file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
