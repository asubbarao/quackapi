#!/usr/bin/env python3
"""
Generate CONFORMANCE_REPORT.md from results.jsonl.
"""
import json
import os
from datetime import datetime

RESULTS = os.path.join(os.path.dirname(__file__), "results.jsonl")
REPORT = os.path.join(os.path.dirname(__file__), "CONFORMANCE_REPORT.md")

CLASS_DESCRIPTIONS = {
    "BUG": "quackapi behavior is wrong relative to FastAPI and should be fixed",
    "INTENTIONAL": "documented design difference between quackapi and FastAPI",
    "COSMETIC": "wording / header order difference that does not affect semantics",
    "FASTAPI-QUIRK": "FastAPI behavior that is arguably wrong/surprising (e.g. trailing-slash 307 redirect)",
}


def main():
    with open(RESULTS) as f:
        rows = [json.loads(line) for line in f if line.strip()]

    total = len(rows)
    matches = [r for r in rows if r["verdict"] == "MATCH"]
    diverges = [r for r in rows if r["verdict"] == "DIVERGE"]

    by_class: dict[str, list] = {}
    for r in diverges:
        cls = r.get("class") or "UNKNOWN"
        by_class.setdefault(cls, []).append(r)

    lines = []
    lines.append("# Conformance Report: quackapi vs FastAPI")
    lines.append(f"\nGenerated: {datetime.now().isoformat()}")
    lines.append(f"\nComparison methodology: same HTTP requests replayed against both stacks.")
    lines.append("quackapi runs on the C++ extension server (serve_brain_ex).")
    lines.append("FastAPI runs via uvicorn with a hand-written mirror app.")
    lines.append("\n## Summary\n")
    lines.append(f"| Metric | Count |")
    lines.append(f"|--------|-------|")
    lines.append(f"| Total cases | {total} |")
    lines.append(f"| MATCH | {len(matches)} |")
    lines.append(f"| DIVERGE | {len(diverges)} |")
    for cls in ["BUG", "INTENTIONAL", "COSMETIC", "FASTAPI-QUIRK"]:
        cnt = len(by_class.get(cls, []))
        lines.append(f"| &nbsp;&nbsp;↳ {cls} | {cnt} |")

    lines.append("\n## Classification Key\n")
    for cls, desc in CLASS_DESCRIPTIONS.items():
        lines.append(f"- **{cls}**: {desc}")

    # BUG section (most important)
    bugs = by_class.get("BUG", [])
    lines.append(f"\n## BUG-class Divergences ({len(bugs)} total)\n")
    if not bugs:
        lines.append("_None — quackapi matches FastAPI on all non-intentional cases._")
    else:
        lines.append("These are cases where quackapi returns a different result and should be fixed.\n")
        for r in bugs:
            lines.append(f"### `{r['id']}` — {r['method']} {r['path']}")
            lines.append(f"- **Notes**: {r['notes']}")
            lines.append(f"- **quackapi**: status={r['qk_status']} ct={r['qk_ct']}")
            lines.append(f"  ```")
            lines.append(f"  {(r['qk_body'] or '')[:300]}")
            lines.append(f"  ```")
            lines.append(f"- **FastAPI**: status={r['fa_status']} ct={r['fa_ct']}")
            lines.append(f"  ```")
            lines.append(f"  {(r['fa_body'] or '')[:300]}")
            lines.append(f"  ```")
            lines.append("")

    # INTENTIONAL section
    intentional = by_class.get("INTENTIONAL", [])
    lines.append(f"\n## INTENTIONAL Divergences ({len(intentional)} total)\n")
    if not intentional:
        lines.append("_None._")
    else:
        for r in intentional:
            lines.append(f"### `{r['id']}` — {r['method']} {r['path']}")
            lines.append(f"- **Notes**: {r['notes']}")
            lines.append(f"- qk: {r['qk_status']} | fa: {r['fa_status']}")
            lines.append("")

    # COSMETIC section
    cosmetic = by_class.get("COSMETIC", [])
    lines.append(f"\n## COSMETIC Divergences ({len(cosmetic)} total)\n")
    if not cosmetic:
        lines.append("_None._")
    else:
        lines.append("| Case ID | Method | Path | Notes |")
        lines.append("|---------|--------|------|-------|")
        for r in cosmetic:
            lines.append(f"| `{r['id']}` | {r['method']} | `{r['path']}` | {r['notes'][:100]} |")

    # FASTAPI-QUIRK section
    quirks = by_class.get("FASTAPI-QUIRK", [])
    lines.append(f"\n## FASTAPI-QUIRK Divergences ({len(quirks)} total)\n")
    if not quirks:
        lines.append("_None._")
    else:
        for r in quirks:
            lines.append(f"### `{r['id']}` — {r['method']} {r['path']}")
            lines.append(f"- **Notes**: {r['notes']}")
            lines.append(f"- qk: {r['qk_status']} | fa: {r['fa_status']}")
            lines.append(f"- fa body: `{(r['fa_body'] or '')[:150]}`")
            lines.append("")

    # Full match list
    lines.append(f"\n## Matching Cases ({len(matches)} total)\n")
    lines.append("| Case ID | Method | Path |")
    lines.append("|---------|--------|------|")
    for r in matches:
        lines.append(f"| `{r['id']}` | {r['method']} | `{r['path']}` |")

    # Rerun command
    lines.append("\n## Re-run Command\n")
    lines.append("```bash")
    lines.append("cd /Users/aloksubbarao/quackapi")
    lines.append("bash test/conformance/run_conformance.sh")
    lines.append("```")
    lines.append("\nOr to run driver only against existing servers:")
    lines.append("```bash")
    lines.append("cd test/conformance")
    lines.append("python3 driver.py --qk http://127.0.0.1:18500 --fa http://127.0.0.1:18501")
    lines.append("python3 generate_report.py")
    lines.append("```")

    with open(REPORT, "w") as f:
        f.write("\n".join(lines) + "\n")

    print(f"Report written to {REPORT}")
    print(f"Total: {total}, Match: {len(matches)}, Diverge: {len(diverges)}")
    for cls, rows_cls in sorted(by_class.items()):
        print(f"  {cls}: {len(rows_cls)}")


if __name__ == "__main__":
    main()
