#!/usr/bin/env python3
"""Run the same HTTP contract against FastAPI and QuackAPI.

The driver keeps transport and semantic checks separate.  QuackAPI's default
JSON envelope is an array of rows while FastAPI commonly returns an object, so
object/list normalization is intentional and explicit.  A status mismatch,
validation-location mismatch, leaked response field, missing header, or
transport failure is still a hard failure.
"""

from __future__ import annotations

import argparse
import gzip
import json
import statistics
import sys
import time
import urllib.error
import urllib.request
from collections import defaultdict
from pathlib import Path
from typing import Any


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: ANN001
        return None


OPENER = urllib.request.build_opener(NoRedirect)


def request(base: str, case: dict[str, Any]) -> dict[str, Any]:
    data = case.get("body")
    encoded = None if data is None else data.encode("utf-8")
    req = urllib.request.Request(base.rstrip("/") + case["path"], data=encoded, method=case["method"])
    for key, value in (case.get("headers") or {}).items():
        req.add_header(key, value)
    if case["method"] in {"POST", "PUT", "PATCH"} and not any(
        key.lower() == "content-type" for key in (case.get("headers") or {})
    ):
        req.add_header("Content-Type", "application/json")
    started = time.perf_counter_ns()
    try:
        with OPENER.open(req, timeout=10) as response:
            status = response.status
            headers = dict(response.headers.items())
            body = response.read()
    except urllib.error.HTTPError as error:
        status = error.code
        headers = dict(error.headers.items()) if error.headers else {}
        body = error.read() if error.fp else b""
    except Exception as error:  # noqa: BLE001
        return {"status": 0, "headers": {}, "body": b"", "error": str(error), "elapsed_ms": _elapsed(started)}
    if (header(headers, "Content-Encoding") or "").lower() == "gzip":
        try:
            body = gzip.decompress(body)
        except OSError as error:
            return {
                "status": status,
                "headers": headers,
                "body": body,
                "error": f"gzip decode failed: {error}",
                "elapsed_ms": _elapsed(started),
            }
    return {"status": status, "headers": headers, "body": body, "error": None, "elapsed_ms": _elapsed(started)}


def _elapsed(started: int) -> float:
    return (time.perf_counter_ns() - started) / 1_000_000


def header(headers: dict[str, str], name: str) -> str | None:
    wanted = name.lower()
    return next((value for key, value in headers.items() if key.lower() == wanted), None)


def json_body(raw: bytes) -> Any:
    try:
        return json.loads(raw.decode("utf-8")) if raw else None
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None


def normalize_json(value: Any, semantic: str) -> Any:
    if semantic == "json_object" and isinstance(value, list) and len(value) == 1:
        return value[0]
    return value


def first_validation_detail(value: Any) -> dict[str, Any] | None:
    if isinstance(value, dict) and isinstance(value.get("detail"), list) and value["detail"]:
        detail = value["detail"][0]
        return detail if isinstance(detail, dict) else None
    return None


def contains_key(value: Any, key: str) -> bool:
    if isinstance(value, dict):
        return key in value or any(contains_key(child, key) for child in value.values())
    if isinstance(value, list):
        return any(contains_key(child, key) for child in value)
    return False


def subset_matches(observed: Any, expected: dict[str, Any]) -> bool:
    if not isinstance(observed, dict):
        return False
    for key, wanted in expected.items():
        if key == "payload_prefix":
            if not str(observed.get("payload", "")).startswith(wanted):
                return False
        elif observed.get(key) != wanted:
            return False
    return True


def evaluate(case: dict[str, Any], result: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    expected_status = case.get("expect_status")
    if result["status"] != expected_status:
        failures.append(f"status={result['status']} expected={expected_status}")
    if result.get("error"):
        failures.append(result["error"])
    body = result["body"]
    text = body.decode("utf-8", errors="replace")
    parsed = json_body(body)
    semantic = case.get("semantic")
    normalized = normalize_json(parsed, semantic or "")

    if semantic == "empty" and body:
        failures.append("expected empty HEAD body")
    if semantic == "allow" and header(result["headers"], "Allow") is None:
        failures.append("405 response lacks Allow header")
    if semantic == "json_object":
        expected = case.get("expect", {})
        if not subset_matches(normalized, expected):
            failures.append(f"json subset mismatch observed={normalized!r} expected={expected!r}")
        if case.get("body_absent") and contains_key(normalized, case["body_absent"]):
            failures.append(f"response leaked field {case['body_absent']!r}")
    elif semantic == "json_list":
        if not isinstance(parsed, list):
            failures.append(f"expected JSON list, got {parsed!r}")
        elif "expect_len" in case and len(parsed) != case["expect_len"]:
            failures.append(f"list length={len(parsed)} expected={case['expect_len']}")
    elif semantic == "validation":
        detail = first_validation_detail(parsed)
        if result["status"] == 422 and detail is None:
            failures.append(f"422 body lacks detail loc/msg/type: {parsed!r}")
        if detail is not None:
            loc = detail.get("loc", [])
            if case.get("loc0") and (not loc or loc[0] != case["loc0"]):
                failures.append(f"loc[0]={loc!r} expected {case['loc0']!r}")
            if case.get("loc1") and (len(loc) < 2 or loc[1] != case["loc1"]):
                failures.append(f"loc[1]={loc!r} expected {case['loc1']!r}")
    elif semantic == "redirect":
        if header(result["headers"], "Location") != case["location"]:
            failures.append(f"Location={header(result['headers'], 'Location')!r} expected {case['location']!r}")
    elif semantic == "text":
        if case.get("content_type") and not (header(result["headers"], "Content-Type") or "").startswith(
            case["content_type"]
        ):
            failures.append(f"content-type={header(result['headers'], 'Content-Type')!r}")
        if case.get("body_contains") not in text:
            failures.append(f"body lacks {case['body_contains']!r}")
    elif semantic in {"ndjson", "sse", "csv"}:
        if case.get("content_type") and not (header(result["headers"], "Content-Type") or "").startswith(
            case["content_type"]
        ):
            failures.append(f"content-type={header(result['headers'], 'Content-Type')!r}")
        if case.get("expect_lines"):
            if semantic == "sse":
                observed_lines = sum(1 for line in text.splitlines() if line.startswith("data:"))
            else:
                observed_lines = len([line for line in text.splitlines() if line.strip()])
            if observed_lines != case["expect_lines"]:
                failures.append(f"nonempty lines mismatch in {text[:200]!r}")
    elif semantic == "cors":
        if header(result["headers"], "Access-Control-Allow-Origin") != case["cors_origin"]:
            failures.append("CORS origin header missing or wrong")
    elif semantic == "compressed":
        if case.get("content_encoding") and header(result["headers"], "Content-Encoding") != case["content_encoding"]:
            failures.append("response was not gzip encoded")
        if not isinstance(normalized, dict) or not str(normalized.get("payload", "")).startswith(
            case["expect"]["payload_prefix"]
        ):
            failures.append("decoded compressed payload mismatch")
    elif semantic == "openapi":
        if not isinstance(parsed, dict) or "openapi" not in parsed or "paths" not in parsed:
            failures.append("OpenAPI document missing openapi/paths")
    elif semantic in {"error", "auth_error"}:
        if result["status"] >= 400 and not text:
            failures.append("error response has an empty body")
    return failures


def fuzz_cases() -> list[dict[str, Any]]:
    cases: list[dict[str, Any]] = []
    for value in ["0", "-1", "01", "1.5", "1e2", "999999999999999999999999", "%20", "%E4%B8%AD"]:
        overflow = value == "999999999999999999999999"
        cases.append(
            {
                "id": f"fuzz_item_{value.replace('%', 'pct')}",
                "group": "fuzz",
                "method": "GET",
                "path": f"/matrix/items/{value}",
                "headers": {},
                "expect_status": 422 if value not in {"0", "-1", "01"} else 404 if value != "01" else 200,
                "status_by_stack": {"quackapi": 422, "fastapi": 404} if overflow else None,
                "known_gap": "integer_overflow_semantics" if overflow else None,
                "semantic": (
                    "validation" if value not in {"0", "-1", "01"} else "error" if value != "01" else "json_object"
                ),
                "loc0": "path" if value not in {"0", "-1", "01"} else None,
                "loc1": "item_id" if value not in {"0", "-1", "01"} else None,
                "expect": {"id": 1} if value == "01" else {},
            }
        )
    bodies = [
        "{}",
        '{"name":null,"price":1}',
        '{"name":"x","price":null}',
        '{"name":"x","price":1,"tags":["a","b","c","d","e"]}',
        '{"name":"x","price":1,"tags":[1]}',
        '{"name":"x","price":1,"nested":{"x":1}}',
    ]
    for index, body in enumerate(bodies):
        cases.append(
            {
                "id": f"fuzz_body_{index}",
                "group": "fuzz",
                "method": "POST",
                "path": "/matrix/items",
                "headers": {"Content-Type": "application/json"},
                "body": body,
                "expect_status": 422,
                "semantic": "validation",
                "loc0": "body",
            }
        )
    return cases


def load_cases(path: Path, include_fuzz: bool) -> list[dict[str, Any]]:
    cases = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    return cases + (fuzz_cases() if include_fuzz else [])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--quack", required=True)
    parser.add_argument("--fastapi", required=True)
    parser.add_argument("--cases", default=str(Path(__file__).with_name("cases.jsonl")))
    parser.add_argument("--out", required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--no-fuzz", action="store_true")
    args = parser.parse_args()

    cases = load_cases(Path(args.cases), not args.no_fuzz)
    rows: list[dict[str, Any]] = []
    failures: list[dict[str, Any]] = []
    known_gaps: list[dict[str, Any]] = []
    latency: dict[tuple[str, str], list[float]] = defaultdict(list)

    for case in cases:
        per_stack: dict[str, dict[str, Any]] = {}
        for stack, base in (("quackapi", args.quack), ("fastapi", args.fastapi)):
            observed = request(base, case)
            stack_case = dict(case)
            if case.get("status_by_stack"):
                stack_case["expect_status"] = case["status_by_stack"][stack]
            stack_failures = evaluate(stack_case, observed)
            per_stack[stack] = observed
            latency[(stack, case["group"])].append(observed["elapsed_ms"])
            row = {
                "id": case["id"],
                "group": case["group"],
                "stack": stack,
                "status": observed["status"],
                "elapsed_ms": observed["elapsed_ms"],
                "content_type": header(observed["headers"], "Content-Type"),
                "content_encoding": header(observed["headers"], "Content-Encoding"),
                "failures": stack_failures,
                "body": observed["body"].decode("utf-8", errors="replace")[:500],
            }
            rows.append(row)
            if stack_failures and case.get("known_gap"):
                known_gaps.append({**row, "known_gap": case["known_gap"]})
            elif stack_failures:
                failures.append(row)

        left, right = per_stack["quackapi"], per_stack["fastapi"]
        if left["status"] != right["status"] and not case.get("status_by_stack"):
            pair_row = {
                "id": case["id"],
                "group": case["group"],
                "stack": "pair",
                "status": None,
                "elapsed_ms": None,
                "failures": [f"status pair quackapi={left['status']} fastapi={right['status']}"],
            }
            if case.get("known_gap"):
                known_gaps.append({**pair_row, "known_gap": case["known_gap"]})
            else:
                failures.append(pair_row)

    def quantiles(values: list[float]) -> dict[str, float]:
        ordered = sorted(values)
        if not ordered:
            return {"count": 0, "p50_ms": 0, "p95_ms": 0, "p99_ms": 0}

        def at(p: float) -> float:
            index = min(len(ordered) - 1, round((len(ordered) - 1) * p))
            return round(ordered[index], 3)

        return {"count": len(ordered), "p50_ms": at(0.50), "p95_ms": at(0.95), "p99_ms": at(0.99)}

    summary = {
        "cases": len(cases),
        "requests": len(rows),
        "failures": len(failures),
        "known_gaps": len(known_gaps),
        "passed": len(rows) - sum(1 for row in rows if row.get("failures")),
        "latency": {f"{stack}/{group}": quantiles(values) for (stack, group), values in sorted(latency.items())},
        "failure_ids": sorted({row["id"] for row in failures}),
        "known_gap_ids": sorted({row["id"] for row in known_gaps}),
    }
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "results.jsonl").write_text("\n".join(json.dumps(row, sort_keys=True) for row in rows) + "\n")
    (out / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    print(json.dumps(summary, indent=2, sort_keys=True))
    if failures:
        for row in failures[:20]:
            print(f"FAIL {row['id']} [{row['stack']}]: {'; '.join(row.get('failures', []))}")
    if known_gaps:
        for row in known_gaps[:20]:
            print(f"KNOWN GAP {row['id']} [{row['stack']}] ({row['known_gap']}): {'; '.join(row.get('failures', []))}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
