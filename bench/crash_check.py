#!/usr/bin/env python3
"""Reconcile the crash case: what the client was told is durable against what
survived SIGKILL.

This lived inline in run.sh, where it could only ever write a row. It is a
program so the reconciliation has an exit code of its own and can be exercised
directly by bench/test_bench_config.py.
"""
from __future__ import annotations

import argparse
import json
import sys


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stack", required=True)
    # The durable stack is the claim under test. The volatile stack is the
    # control: losing its in-flight jobs is the measurement, not a regression.
    parser.add_argument("--durability", required=True, choices=["durable", "volatile"])
    parser.add_argument("--attempted", type=int, required=True)
    parser.add_argument("--acknowledged", type=int, required=True)
    parser.add_argument("--survived", type=int, required=True)
    parser.add_argument("--client-json", required=True)
    args = parser.parse_args()
    lost = args.acknowledged - args.survived
    print(json.dumps({
        "stack": args.stack, "durability": args.durability, "attempted": args.attempted,
        "acknowledged": args.acknowledged, "survived": args.survived,
        "acknowledged_but_not_surviving": lost, "client": json.loads(args.client_json),
    }, separators=(",", ":")))
    if args.durability == "durable" and lost > 0:
        print(f"FAIL: {args.stack} lost {lost} acknowledged job(s) across the crash", file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
