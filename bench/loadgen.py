#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import csv
import gzip
import json
import math
import sys
import time
from collections import Counter
from pathlib import Path
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


def unwrap(payload: object) -> object:
    """quackapi answers with a row array, FastAPI with a bare object."""
    return payload[0] if isinstance(payload, list) and payload else payload


def whole_number(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def contract(
    check: str, expected: object, status: int, raw: bytes, elapsed_ms: float
) -> tuple[bool, float | None, int | None]:
    """Decide whether one response honoured the contract.

    Returns (ok, overhead_ms, out_tokens). overhead_ms is the end-to-end latency
    minus the upstream's own reported service time, and is deliberately signed:
    clamping a negative residual would hide clock, serialization, or upstream
    timing inconsistencies rather than surface them.
    """
    if status != 200:
        return False, None, None
    try:
        payload = json.loads(raw)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return False, None, None
    if check == "exact":
        return payload == expected, None, None
    body = unwrap(payload)
    if not isinstance(body, dict):
        return False, None, None
    if check == "embedding":
        return whole_number(body.get("dims")) and body["dims"] > 0, None, None
    # generation: a model that returned no text, or reported no service time of
    # its own, leaves nothing to measure the gateway against.
    if not isinstance(body.get("response"), str):
        return False, None, None
    upstream_ms = body.get("ollama_total_ms")
    if (
        isinstance(upstream_ms, bool)
        or not isinstance(upstream_ms, (int, float))
        or upstream_ms <= 0
    ):
        return False, None, None
    out_tokens = body.get("out_tokens")
    return (
        True,
        elapsed_ms - float(upstream_ms),
        out_tokens if whole_number(out_tokens) and out_tokens >= 0 else None,
    )


def percentile(values: list[float], q: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, math.ceil(q * len(ordered)) - 1)]


async def phase(
    host: str,
    port: int,
    target: str,
    concurrency: int,
    duration: float,
    timeout: float,
    expected: object,
    method: str,
    check: str,
) -> list[dict]:
    deadline = time.perf_counter() + duration
    attempts: list[dict] = []

    async def worker() -> None:
        reader: asyncio.StreamReader | None = None
        writer: asyncio.StreamWriter | None = None
        while time.perf_counter() < deadline:
            started = time.perf_counter_ns()
            try:
                if writer is None:
                    reader, writer = await asyncio.wait_for(
                        asyncio.open_connection(host, port), timeout
                    )
                request = (
                    f"{method} {target} HTTP/1.1\r\nHost: {host}:{port}\r\n"
                    "Accept: application/json\r\nContent-Length: 0\r\n"
                    "Connection: keep-alive\r\n\r\n"
                ).encode()
                writer.write(request)
                await asyncio.wait_for(writer.drain(), timeout)
                assert reader is not None
                status, body, close = await asyncio.wait_for(
                    read_response(reader), timeout
                )
                elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
                contract_ok, overhead_ms, out_tokens = contract(
                    check, expected, status, body, elapsed_ms
                )
                attempts.append(
                    {
                        "latency_ms": elapsed_ms,
                        "status": status,
                        "ok": contract_ok,
                        "error": ""
                        if contract_ok
                        else ("shed" if status == SHED_STATUS else "contract"),
                        "overhead_ms": overhead_ms,
                        "out_tokens": out_tokens,
                    }
                )
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
                attempts.append(
                    {
                        "latency_ms": elapsed_ms,
                        "status": None,
                        "ok": False,
                        "error": name,
                        "overhead_ms": None,
                        "out_tokens": None,
                    }
                )
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
    expected = json.loads(args.expect_json) if args.expect_json else None
    if args.warmup > 0:
        await phase(
            host, port, target, args.concurrency, args.warmup, args.timeout,
            expected, args.method, args.check,
        )
    started = time.perf_counter()
    attempts = await phase(
        host, port, target, args.concurrency, args.duration, args.timeout,
        expected, args.method, args.check,
    )
    elapsed = time.perf_counter() - started
    successful = [row["latency_ms"] for row in attempts if row["ok"]]
    response_latencies = [
        row["latency_ms"] for row in attempts if row["status"] is not None
    ]
    errors = Counter(row["error"] for row in attempts if row["error"])
    statuses = Counter(
        str(row["status"]) for row in attempts if row["status"] is not None
    )
    args.raw.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(args.raw, "wt", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=["latency_ms", "status", "ok", "error", "overhead_ms", "out_tokens"],
        )
        writer.writeheader()
        writer.writerows(attempts)
    # The gateway's own cost, with the model's reported service time removed.
    # Only the generation check produces it.
    overheads = [row["overhead_ms"] for row in attempts if row["overhead_ms"] is not None]
    produced_tokens = [row["out_tokens"] for row in attempts if row["out_tokens"] is not None]
    contract_failures = errors.pop("contract", 0)
    sheds = errors.pop("shed", 0)
    timeouts = errors.pop("timeout", 0)
    resets = errors.pop("reset", 0)
    eofs = errors.pop("eof", 0)
    return {
        "stack": args.stack,
        "trial": args.trial,
        "concurrency": args.concurrency,
        "configured_duration_sec": args.duration,
        "elapsed_sec": elapsed,
        "attempted": len(attempts),
        "successful": len(successful),
        "successful_rps": len(successful) / elapsed if elapsed else 0,
        "p50_ms": percentile(successful, 0.50),
        "p99_ms": percentile(successful, 0.99),
        "max_ms": max(successful) if successful else None,
        "all_response_max_ms": max(response_latencies) if response_latencies else None,
        "overhead_p50_ms": percentile(overheads, 0.50),
        "overhead_p99_ms": percentile(overheads, 0.99),
        "overhead_samples": len(overheads),
        "out_tokens": sum(produced_tokens) if produced_tokens else None,
        "http_statuses": dict(statuses),
        "contract_failures": contract_failures,
        "shed": sheds,
        "timeouts": timeouts,
        "resets": resets,
        "eofs": eofs,
        "other_no_response": sum(errors.values()),
        "other_errors": dict(errors),
        "raw": str(args.raw),
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
    parser.add_argument("--method", default="GET", choices=["GET", "POST"])
    # exact       -- the whole body equals --expect-json
    # embedding   -- POST /llm/embed answered with a positive integer dims
    # generation  -- POST /llm/ask answered with response text and a positive
    #                upstream service time, which yields the signed overhead
    parser.add_argument("--check", default="exact", choices=["exact", "embedding", "generation"])
    parser.add_argument("--expect-json")
    parser.add_argument("--raw", type=Path, required=True)
    args = parser.parse_args()
    if args.check == "exact" and args.expect_json is None:
        parser.error("--expect-json is required when --check is exact")
    summary = asyncio.run(run(args))
    print(json.dumps(summary, separators=(",", ":")))
    reasons = []
    if summary["contract_failures"]:
        reasons.append(
            f"{summary['contract_failures']} responses were not the contracted 200 body"
        )
    if not summary["successful"]:
        reasons.append(f"no request of {summary['attempted']} completed the contract")
    if reasons:
        print(
            f"FAIL: {args.stack} c{args.concurrency} trial{args.trial}: {'; '.join(reasons)}",
            file=sys.stderr,
        )
        raise SystemExit(1)


if __name__ == "__main__":
    main()
