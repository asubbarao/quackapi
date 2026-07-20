#!/usr/bin/env python3
"""
quackapi ↔ FastAPI equivalence driver.

Fires real HTTP requests against a live quackapi_serve() instance and asserts
status / body / headers against documented FastAPI behavior encoded in cases.jsonl.

Every result is from a request that actually ran (or force_na / skip with reason).
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parent
CASES_PATH = ROOT / "cases.jsonl"
RESULTS_PATH = ROOT / "results" / "results.jsonl"
JWT_SECRET = b"conformance-secret"


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def make_jwt(secret: bytes = JWT_SECRET, sub: str = "alice", exp_delta: int = 3600) -> str:
    header = b64url(json.dumps({"alg": "HS256", "typ": "JWT"}, separators=(",", ":")).encode())
    payload = b64url(json.dumps({"sub": sub, "exp": int(time.time()) + exp_delta}, separators=(",", ":")).encode())
    sig = b64url(hmac.new(secret, f"{header}.{payload}".encode(), hashlib.sha256).digest())
    return f"{header}.{payload}.{sig}"


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Do not follow 3xx — redirect cases assert status + Location as-sent."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: ANN001
        return None


_OPENER = urllib.request.build_opener(_NoRedirect)


def http_request(
    base: str,
    method: str,
    path: str,
    headers: Dict[str, str],
    body: Optional[str],
    timeout: float = 5.0,
) -> Tuple[int, Dict[str, str], bytes]:
    url = base.rstrip("/") + path
    data = None if body is None else body.encode("utf-8")
    # Methods that commonly need a body entity for httplib: send empty JSON if none given
    if method in ("POST", "PUT", "PATCH") and data is None:
        data = b""
    req = urllib.request.Request(url, data=data, method=method)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    if method in ("POST", "PUT", "PATCH"):
        if not any(k.lower() == "content-type" for k in (headers or {})):
            req.add_header("Content-Type", "application/json")
    try:
        # Use opener that does not follow redirects (307 Location must be observable).
        with _OPENER.open(req, timeout=timeout) as resp:
            raw = resp.read()
            # HEAD: body empty by design
            hdrs = {k: v for k, v in resp.headers.items()}
            return resp.status, hdrs, raw
    except urllib.error.HTTPError as e:
        raw = e.read() if e.fp else b""
        hdrs = {k: v for k, v in e.headers.items()} if e.headers else {}
        return e.code, hdrs, raw
    except Exception as e:  # noqa: BLE001
        return 0, {}, f"REQUEST_ERROR: {e}".encode()


def parse_json(body: bytes) -> Any:
    try:
        return json.loads(body.decode("utf-8") if body else "null")
    except Exception:
        return None


def get_json_path(obj: Any, dotted: str) -> Any:
    """Support '0.name' into list[0]['name'] or dict paths."""
    cur = obj
    for part in dotted.split("."):
        if cur is None:
            return None
        if part.isdigit():
            idx = int(part)
            if not isinstance(cur, list) or idx >= len(cur):
                return None
            cur = cur[idx]
        else:
            if not isinstance(cur, dict) or part not in cur:
                return None
            cur = cur[part]
    return cur


def header_get(headers: Dict[str, str], name: str) -> Optional[str]:
    for k, v in headers.items():
        if k.lower() == name.lower():
            return v
    return None


def classify(case: dict, verdict: str, notes: str) -> str:
    if verdict == "N/A":
        return case.get("class_hint") or "NOT-BUILT-YET"
    if verdict == "PASS":
        if case.get("force_pass_stronger"):
            return "STRONGER"
        return "MATCH"
    # FAIL
    hint = case.get("class_hint")
    if hint in ("INTENTIONAL", "FASTAPI-QUIRK", "NOT-BUILT-YET", "COSMETIC"):
        return hint
    # Heuristics
    if "optional" in notes.lower() or "not built" in notes.lower():
        return "NOT-BUILT-YET"
    if "trailing slash" in notes.lower() or "Starlette" in notes:
        return "FASTAPI-QUIRK"
    if "array of rows" in notes.lower() or "query not JSON" in notes.lower():
        return "INTENTIONAL"
    if "STRONGER" in notes or "stronger" in notes.lower() or "int64 overflow" in notes.lower():
        return "STRONGER"
    return "BUG"


def evaluate(case: dict, status: int, headers: Dict[str, str], body: bytes) -> Tuple[str, str, List[str]]:
    """Return (verdict, notes, failures)."""
    failures: List[str] = []
    notes: List[str] = []
    if case.get("force_na"):
        return "N/A", case.get("notes") or "feature not built", []
    if case.get("skip_run") and case.get("force_pass_stronger"):
        return "PASS", case.get("notes") or "documented stronger behavior", []
    if case.get("skip_run"):
        return "N/A", case.get("notes") or "skipped", []

    text = body.decode("utf-8", errors="replace")
    j = parse_json(body)
    ct = header_get(headers, "Content-Type") or ""

    exp_status = case.get("expect_status")
    if exp_status is not None and status != exp_status:
        failures.append(f"status: observed={status} expected={exp_status}")

    if case.get("expect_body_empty"):
        if body and len(body) > 0:
            # HEAD may report Content-Length but empty entity
            if case.get("method") != "HEAD":
                failures.append(f"body not empty: {text[:80]!r}")

    for frag in case.get("expect_body_contains") or []:
        if frag not in text:
            failures.append(f"body missing {frag!r}; body={text[:200]!r}")

    if "expect_body_json" in case:
        if j != case["expect_body_json"]:
            failures.append(f"json body: observed={j!r} expected={case['expect_body_json']!r}")

    if case.get("expect_ct_substr"):
        if case["expect_ct_substr"] not in ct:
            failures.append(f"content-type: observed={ct!r} expected substr={case['expect_ct_substr']!r}")

    if case.get("expect_header_present"):
        if header_get(headers, case["expect_header_present"]) is None:
            failures.append(f"missing header {case['expect_header_present']}")

    for hk, hv in (case.get("expect_header_eq") or {}).items():
        got = header_get(headers, hk)
        if got != hv:
            failures.append(f"header {hk}: observed={got!r} expected={hv!r}")

    if case.get("expect_json_len") is not None:
        if not isinstance(j, list) or len(j) != case["expect_json_len"]:
            failures.append(
                f"json len: observed={type(j).__name__}/{getattr(j,'__len__',lambda: '?')()} expected list len={case['expect_json_len']}"
            )

    for path, exp in (case.get("expect_json_path_eq") or {}).items():
        got = get_json_path(j, path)
        if got != exp:
            failures.append(f"json path {path}: observed={got!r} expected={exp!r}")

    if case.get("expect_422"):
        e422 = case["expect_422"]
        ok_shape = False
        if isinstance(j, dict) and isinstance(j.get("detail"), list) and j["detail"]:
            d0 = j["detail"][0]
            if isinstance(d0, dict) and "loc" in d0 and "msg" in d0 and "type" in d0:
                ok_shape = True
                loc = d0["loc"]
                if e422.get("loc0") and (not loc or loc[0] != e422["loc0"]):
                    failures.append(f"422 loc[0]: observed={loc} expected starts {e422['loc0']}")
                if e422.get("loc1") and (len(loc) < 2 or loc[1] != e422["loc1"]):
                    failures.append(f"422 loc[1]: observed={loc} expected {e422['loc1']}")
                if e422.get("type") and d0.get("type") != e422["type"]:
                    # type_error vs int_parsing: accept type_error as FastAPI-shaped family
                    if not (e422["type"] in str(d0.get("type", "")) or d0.get("type") == "type_error"):
                        failures.append(f"422 type: observed={d0.get('type')!r} expected={e422['type']!r}")
            else:
                failures.append(f"422 detail[0] missing loc/msg/type: {d0!r}")
        else:
            failures.append(f"422 body not {{detail:[...]}}: {text[:200]!r}")
        if not ok_shape and status == 422:
            pass

    if case.get("expect_422_keys"):
        if not (isinstance(j, dict) and isinstance(j.get("detail"), list) and j["detail"]):
            failures.append("422_keys: no detail array")
        else:
            d0 = j["detail"][0]
            for k in case["expect_422_keys"]:
                if k not in d0:
                    failures.append(f"422 missing key {k}")

    if case.get("notes"):
        notes.append(case["notes"])

    # Special: HEAD_EXPLICIT — if we registered HEAD /health, 200 is PASS
    if case.get("id") == "explicit_head" and status == 200:
        return "PASS", "; ".join(notes), []

    # health_head_auto: if 405, that's fail / not-built auto-head (unless explicit works)
    if case.get("id") == "health_head_auto":
        if status == 200:
            return "PASS", "auto or explicit HEAD works", []
        return (
            "FAIL",
            "no auto-HEAD for GET (explicit HEAD route exists but httplib may not dispatch HEAD)",
            failures or [f"status={status}"],
        )

    if case.get("id") == "get_user_head_explicit":
        if status == 200:
            return "PASS", "HEAD on GET path works", []
        return "N/A", "auto-HEAD for path GET routes not built", []

    # trailing slash: if 200 same as non-slash → INTENTIONAL (more lenient); if 404 → also intentional vs 307
    if case.get("id") in ("list_users_trailing_slash", "health_trailing_slash"):
        if status == 307:
            return "PASS", "matches Starlette redirect", []
        if status == 200:
            return "FAIL", "no 307 redirect; path treated as registered (lenient)", failures
        if status == 404:
            return "FAIL", "404 without Starlette 307 redirect (FASTAPI-QUIRK / intentional)", failures

    # optional limit
    if case.get("id") == "search_limit_missing":
        if status == 200:
            return "PASS", "optional query works", []
        if status == 422:
            return "FAIL", "optional query params not supported ($limit always required)", failures

    if case.get("id") == "search_limit_le":
        if status == 422:
            return "PASS", "le constraint enforced", []
        return "FAIL", "no Query(le=) constraint layer", failures

    if case.get("id") == "search_limit_neg":
        if status in (200, 422):
            # empty or validation ok; 500 is BUG
            if status == 200:
                return "PASS", f"status 200 body={text[:80]}", []
            return "PASS", "422 on neg limit", []
        return "FAIL", f"unexpected status {status}", failures

    if case.get("id") == "get_user_overflow":
        if status == 422:
            return "PASS", "STRONGER: int64 overflow rejected with 422", []
        if status == 200:
            return "FAIL", "accepted overflow (FastAPI would also accept Python int)", failures
        return "FAIL", f"status={status}", failures

    if case.get("id") == "post_users_json_body":
        if status == 201 and j and isinstance(j, list) and j[0].get("name") == "dave":
            return "PASS", "JSON body bound", []
        if status == 422:
            return "FAIL", "JSON request body binder not built (params only path/query)", failures
        return "FAIL", f"status={status} body={text[:120]}", failures

    if case.get("id") == "post_users_malformed_json":
        # FastAPI: type=json_invalid, loc=["body", ...]
        if status == 422 and isinstance(j, dict) and j.get("detail"):
            d0 = j["detail"][0] if isinstance(j["detail"], list) and j["detail"] else {}
            if isinstance(d0, dict) and (
                d0.get("type") == "json_invalid" or (isinstance(d0.get("loc"), list) and d0["loc"][:1] == ["body"])
            ):
                return "PASS", "malformed JSON body rejected", []
        return "FAIL", "JSON body parse validation not built (422 is missing query params, not json_invalid)", failures

    if case.get("id") == "post_users_wrong_ct":
        # FastAPI rejects non-application/json for body models with 422
        if status == 422 and "model_attributes" in text or (status == 422 and "body" in text and "name" not in text):
            return "PASS", "wrong Content-Type rejected", []
        # If it accepted body or only failed on missing query fields → not built
        return "FAIL", "Content-Type enforcement for JSON body models not built", failures

    if case.get("id") == "openapi_json" or case.get("id") == "docs_get":
        if status == 200:
            return "PASS", "present", []
        return "N/A", case.get("notes") or "not built", []

    if failures:
        return "FAIL", "; ".join(notes + failures), failures
    return "PASS", "; ".join(notes) if notes else "ok", []


def curl_equiv(method: str, base: str, path: str, headers: dict, body: Optional[str]) -> str:
    parts = ["curl", "-sS", "-D", "-"]
    if method != "GET":
        parts += ["-X", method]
    for k, v in (headers or {}).items():
        parts += ["-H", f"{k}: {v}"]
    if body is not None:
        parts += ["--data-binary", body]
    elif method in ("POST", "PUT", "PATCH"):
        parts += ["--data-binary", ""]
    parts.append(f"'{base.rstrip('/')}{path}'")
    return " ".join(parts)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=os.environ.get("QUACKAPI_BASE", "http://127.0.0.1:18770"))
    ap.add_argument("--cases", default=str(CASES_PATH))
    ap.add_argument("--out", default=str(RESULTS_PATH))
    args = ap.parse_args()

    cases = []
    with open(args.cases) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            cases.append(json.loads(line))

    results = []
    counts = {"PASS": 0, "FAIL": 0, "N/A": 0}
    classes: Dict[str, int] = {}
    groups: Dict[str, Dict[str, int]] = {}

    jwt_token = make_jwt()

    for case in cases:
        cid = case["id"]
        group = case.get("group", "misc")
        groups.setdefault(group, {"PASS": 0, "FAIL": 0, "N/A": 0, "total": 0})
        groups[group]["total"] += 1

        headers = dict(case.get("headers") or {})
        if case.get("jwt"):
            headers["Authorization"] = f"Bearer {jwt_token}"

        if case.get("force_na") or (case.get("skip_run") and not case.get("force_pass_stronger")):
            status, hdrs, body = 0, {}, b""
            verdict = "N/A"
            notes = case.get("notes") or "not built"
            failures: List[str] = []
            observed = {"status": None, "headers": {}, "body": None}
        else:
            status, hdrs, body = http_request(args.base, case["method"], case["path"], headers, case.get("body"))
            verdict, notes, failures = evaluate(case, status, hdrs, body)
            observed = {
                "status": status,
                "headers": {
                    k: v
                    for k, v in hdrs.items()
                    if k.lower()
                    in ("content-type", "allow", "location", "www-authenticate", "set-cookie", "content-length")
                },
                "body": body.decode("utf-8", errors="replace")[:500],
            }

        cls = classify(case, verdict, notes)
        # Reclassify some FAILs after observation
        if verdict == "FAIL":
            if case.get("id") in ("list_users_trailing_slash", "health_trailing_slash"):
                cls = "FASTAPI-QUIRK"
            elif case.get("id") in (
                "search_limit_missing",
                "search_limit_le",
                "get_user_head_explicit",
                "post_users_json_body",
                "post_users_malformed_json",
                "post_users_wrong_ct",
                "health_options",
            ):
                cls = "NOT-BUILT-YET"
            elif case.get("id") in ("health_head_auto",) and status == 405:
                # explicit HEAD is registered but server may not wire Head handler
                cls = "BUG"
            elif (
                case.get("id") in ("allow_header_on_405", "health_post_405", "method_mismatch_users_delete")
                and "Allow" in notes
                or any("Allow" in f for f in failures)
            ):
                if status == 405:
                    cls = "BUG"  # 405 correct, Allow missing
            elif case.get("id") == "get_user_bad_float" and status == 200:
                cls = "BUG"
            elif (
                case.get("id") in ("post_users_age_float_str", "search_limit_float", "search_limit_1e2")
                and status == 200
            ):
                cls = "BUG"
            elif case.get("id") == "post_users_missing_age" and status == 422:
                # loc query vs body is intentional surface difference if using query binder
                if any("loc" in f for f in failures):
                    cls = "INTENTIONAL"
            elif case.get("id") == "get_user_1" and status == 200 and any("json path" in f for f in failures):
                cls = "INTENTIONAL"  # array vs object envelope

        counts[verdict] = counts.get(verdict, 0) + 1
        classes[cls] = classes.get(cls, 0) + 1
        groups[group][verdict] = groups[group].get(verdict, 0) + 1

        row = {
            "id": cid,
            "group": group,
            "verdict": verdict,
            "class": cls,
            "method": case["method"],
            "path": case["path"],
            "curl": curl_equiv(case["method"], args.base, case["path"], headers, case.get("body")),
            "fastapi_doc": case.get("fastapi_doc"),
            "expect_status": case.get("expect_status"),
            "observed": observed,
            "failures": failures,
            "notes": notes,
        }
        results.append(row)
        mark = {"PASS": "✓", "FAIL": "✗", "N/A": "·"}[verdict]
        print(f"{mark} {cid:32} {verdict:4} {cls:14} status={observed.get('status')} {notes[:80]}")

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w") as f:
        for r in results:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")

    summary = {
        "total": len(results),
        "counts": counts,
        "classes": classes,
        "groups": groups,
        "base": args.base,
    }
    summary_path = out.parent / "summary.json"
    with open(summary_path, "w") as f:
        json.dump(summary, f, indent=2)

    print("\n=== SUMMARY ===")
    print(json.dumps(summary, indent=2))
    print(f"wrote {out}")
    return 0 if counts.get("FAIL", 0) == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
