#!/usr/bin/env python3
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import sys


def send(port: int, job_id: int, delay_ms: int) -> dict:
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    body = json.dumps({"id": job_id, "delay_ms": delay_ms})
    try:
        connection.request("POST", "/bench/jobs?token=jessica", body=body, headers={"Content-Type": "application/json"})
        response = connection.getresponse()
        payload = response.read()
        return {"id": job_id, "status": response.status, "body": payload.decode("utf-8", "replace")}
    except Exception as exc:
        return {"id": job_id, "error": type(exc).__name__}
    finally:
        connection.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--jobs", type=int, required=True)
    parser.add_argument("--delay-ms", type=int, required=True)
    args = parser.parse_args()
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(lambda job_id: send(args.port, job_id, args.delay_ms), range(1, args.jobs + 1)))
    acknowledged = [row["id"] for row in results if row.get("status") == 202]
    print(json.dumps({"attempted": args.jobs, "acknowledged": len(acknowledged), "acknowledged_ids": acknowledged, "failures": [row for row in results if row.get("status") != 202]}, separators=(",", ":")))
    if not acknowledged:
        # With nothing acknowledged there is no durability claim to survive the
        # kill, so the crash case measures nothing and must not report success.
        print(f"FAIL: none of the {args.jobs} jobs was acknowledged with 202", file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
