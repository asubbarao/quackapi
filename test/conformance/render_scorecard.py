#!/usr/bin/env python3
"""Recompute headline numbers from results.jsonl into a short SUMMARY block.

Refuses to render anything it cannot verify belongs to a real, current,
reference-backed run: the results must carry the sha256 of the exact
cases.jsonl on disk right now, the exact case count and id set, an explicit
`reference_reachable: true` from driver.py, and a `started_at` within
--max-age-seconds of now. A leftover, foreign, or partial results file is
refused with the specific check that failed named on stderr — never
silently rendered as if it were current.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections import defaultdict
from datetime import UTC, datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def fail(message: str) -> None:
    raise SystemExit(f"REFUSED: {message}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", default=str(ROOT / "cases.jsonl"))
    ap.add_argument("--results-dir", default=str(ROOT / "results"))
    ap.add_argument(
        "--max-age-seconds",
        type=float,
        default=86400.0,
        help="Refuse a results file whose summary.json started_at is older than this.",
    )
    args = ap.parse_args()

    cases_path = Path(args.cases)
    results_dir = Path(args.results_dir)
    results_path = results_dir / "results.jsonl"
    summary_path = results_dir / "summary.json"

    if not cases_path.exists():
        fail(f"missing {cases_path}")
    if not results_path.exists():
        fail(f"missing {results_path}; run the conformance driver first")
    if not summary_path.exists():
        fail(
            f"missing {summary_path} (results.jsonl with no summary.json is not a run this renderer trusts)"
        )

    cases_bytes = cases_path.read_bytes()
    current_cases_sha256 = hashlib.sha256(cases_bytes).hexdigest()
    current_case_ids = {
        json.loads(line)["id"]
        for line in cases_bytes.decode("utf-8").splitlines()
        if line.strip()
    }

    try:
        summary = json.loads(summary_path.read_text())
    except json.JSONDecodeError as exc:
        fail(f"{summary_path} is not valid JSON: {exc}")

    rows = [json.loads(l) for l in results_path.read_text().splitlines() if l.strip()]
    result_ids = [r["id"] for r in rows]

    # --- identity: this results file must belong to the cases.jsonl on disk now ---
    if summary.get("cases_sha256") != current_cases_sha256:
        fail(
            "results were produced from a different cases.jsonl than the one on disk now "
            f"(summary cases_sha256={summary.get('cases_sha256')!r}, current={current_cases_sha256!r}). "
            "Re-run the driver."
        )

    if summary.get("case_count") != len(current_case_ids):
        fail(
            f"summary case_count={summary.get('case_count')!r} does not match "
            f"the {len(current_case_ids)} cases in {cases_path}"
        )

    if len(result_ids) != len(set(result_ids)):
        fail(f"{results_path} has duplicate case ids — not a single clean run")

    if set(result_ids) != current_case_ids:
        missing = current_case_ids - set(result_ids)
        extra = set(result_ids) - current_case_ids
        fail(
            f"{results_path}'s case ids don't match cases.jsonl on disk "
            f"(missing={sorted(missing)}, extra={sorted(extra)}). Re-run the driver."
        )

    # --- reference: this must be a run that actually contacted FastAPI ---
    if summary.get("reference_reachable") is not True:
        fail(
            f"summary.reference_reachable={summary.get('reference_reachable')!r} — "
            "this was not a run with a live FastAPI reference; it cannot report equivalence"
        )

    if not summary.get("quackapi_base") or not summary.get("fastapi_base"):
        fail(
            "summary is missing quackapi_base/fastapi_base — cannot confirm what this run actually talked to"
        )

    if not summary.get("fastapi_version"):
        fail(
            "summary is missing fastapi_version — cannot confirm the reference was pinned"
        )

    # --- freshness ---
    started_at = summary.get("started_at")
    if not started_at:
        fail("summary is missing started_at — cannot verify freshness")
    try:
        started_dt = datetime.fromisoformat(started_at)
    except ValueError:
        fail(f"summary.started_at={started_at!r} is not a valid ISO timestamp")
    if started_dt.tzinfo is None:
        started_dt = started_dt.replace(tzinfo=UTC)
    age_seconds = (datetime.now(UTC) - started_dt).total_seconds()
    if age_seconds < 0:
        fail(
            f"summary.started_at={started_at!r} is in the future — clock skew or a corrupted summary"
        )
    if age_seconds > args.max_age_seconds:
        fail(
            f"results are {age_seconds:.0f}s old, older than --max-age-seconds={args.max_age_seconds:.0f}. "
            "Re-run the driver; a stale results file is refused, not rendered."
        )

    # --- everything checked out: render ---
    counts: defaultdict[str, int] = defaultdict(int)
    classes: defaultdict[str, int] = defaultdict(int)
    groups: defaultdict[str, defaultdict[str, int]] = defaultdict(
        lambda: defaultdict(int)
    )
    for r in rows:
        counts[r["verdict"]] += 1
        classes[r["class"]] += 1
        groups[r["group"]][r["verdict"]] += 1
    total = len(rows)
    passed = counts["PASS"]

    print(
        f"verified: {total} cases, cases.jsonl sha256={current_cases_sha256[:12]}..., age={age_seconds:.0f}s"
    )
    print(
        f"reference: fastapi={summary['fastapi_version']} at {summary['fastapi_base']}, quackapi at {summary['quackapi_base']}"
    )
    print()
    print(
        f"VERDICT: PASS={counts['PASS']} FAIL={counts['FAIL']} N/A={counts['N/A']} "
        f"total={total} ({100 * passed / total:.1f}% PASS)"
    )
    print("groups:")
    for g, c in sorted(groups.items()):
        t = sum(c.values())
        print(
            f"  {g}: PASS={c['PASS']} FAIL={c['FAIL']} N/A={c['N/A']} ({100 * c['PASS'] / t:.0f}% PASS)"
        )
    print()
    print("class (commentary only — never changes the verdict above):", dict(classes))
    if counts["FAIL"]:
        print()
        print(f"FAIL cases ({counts['FAIL']}):")
        for r in rows:
            if r["verdict"] == "FAIL":
                print(f"  {r['id']:32} class={r['class']:14} {r['notes'][:100]}")

    return 0 if counts["FAIL"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
