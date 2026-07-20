#!/usr/bin/env python3
"""Recompute headline numbers from results.jsonl into a short SUMMARY block."""

import json
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent
results = ROOT / "results" / "results.jsonl"
if not results.exists():
    raise SystemExit(f"missing {results}; run test/conformance/run.sh first")

rows = [json.loads(l) for l in results.read_text().splitlines() if l.strip()]
counts = defaultdict(int)
classes = defaultdict(int)
groups = defaultdict(lambda: defaultdict(int))
for r in rows:
    counts[r["verdict"]] += 1
    classes[r["class"]] += 1
    groups[r["group"]][r["verdict"]] += 1
total = len(rows)
passed = counts["PASS"]
print(
    f"overall FastAPI conformance {passed}/{total} ({100 * passed / total:.1f}%), "
    f"#{classes['BUG']} BUGs, #{classes['NOT-BUILT-YET']} not-built-yet"
)
print("groups:")
for g, c in sorted(groups.items()):
    t = sum(c.values())
    print(f"  {g}: {c['PASS']}/{t} ({100 * c['PASS'] / t:.0f}%)")
print("classes:", dict(classes))
print(f"PASS={counts['PASS']} FAIL={counts['FAIL']} N/A={counts['N/A']} total={total}")
