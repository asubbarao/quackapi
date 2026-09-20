"""QuackAPI vs live FastAPI differential HTTP contract driver.

Every case in cases.jsonl is fired at BOTH a live quackapi_serve() instance
and a live, pinned FastAPI reference app (fastapi_app.py, started by this
driver via uvicorn). The two real responses are compared structurally;
there is no hand-encoded "what FastAPI would say" left anywhere in this
file. If the reference is not reachable at start, or stops responding
mid-run, the run fails loudly (non-zero exit, no results written) rather
than reporting equivalence without evidence.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent
CASES_PATH = ROOT / "cases.jsonl"
RESULTS_PATH = ROOT / "results" / "results.jsonl"
JWT_SECRET = b"conformance-secret"

# Keep in sync with fastapi_app.FASTAPI_PINNED_VERSION / requirements.txt.
# Not imported directly: driver.py must run under plain stdlib even when the
# interpreter driving it has no `fastapi` installed (that's fastapi_app.py's
# own, separately pinned venv — see requirements.txt).
FASTAPI_PINNED_VERSION = "0.141.1"

COMPARABLE_HEADERS = (
    "content-type",
    "allow",
    "location",
    "www-authenticate",
    "set-cookie",
)

# quackapi's validator reports every type mismatch as the single generic
# "type_error"; pydantic v2 reports the specific kind. These are the kinds
# a bare scalar-type mismatch can legitimately map to. Anything else is a
# real mismatch, not a wording difference.
_GENERIC_TYPE_MISMATCH = {"type_error"}
_SCALAR_PARSING_TYPES = {
    "int_parsing",
    "int_type",
    "float_parsing",
    "float_type",
    "bool_parsing",
    "bool_type",
    "string_type",
}

# A FAIL's class is commentary a human wrote in advance about a specific,
# already-understood case (class_hint in cases.jsonl) — never something
# inferred at runtime from notes text or a case id. That keeps a real
# failure from being quietly recast into an accepted category by pattern
# matching its own explanation.
FAIL_CLASSES = {
    "BUG",
    "INTENTIONAL",
    "FASTAPI-QUIRK",
    "NOT-BUILT-YET",
    "COSMETIC",
    "STRONGER",
}

MARKER_ONLY_CASE_IDS = {"openapi_json", "docs_get", "redoc_get"}


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def make_jwt(
    secret: bytes = JWT_SECRET, sub: str = "alice", exp_delta: int = 3600
) -> str:
    header = b64url(
        json.dumps({"alg": "HS256", "typ": "JWT"}, separators=(",", ":")).encode()
    )
    payload = b64url(
        json.dumps(
            {"sub": sub, "exp": int(time.time()) + exp_delta}, separators=(",", ":")
        ).encode()
    )
    sig = b64url(
        hmac.new(secret, f"{header}.{payload}".encode(), hashlib.sha256).digest()
    )
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
    headers: dict[str, str],
    body: str | None,
    timeout: float = 5.0,
) -> tuple[int, dict[str, str], bytes]:
    url = base.rstrip("/") + path
    data = None if body is None else body.encode("utf-8")
    if method in ("POST", "PUT", "PATCH") and data is None:
        data = b""
    req = urllib.request.Request(url, data=data, method=method)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    if method in ("POST", "PUT", "PATCH"):
        if not any(k.lower() == "content-type" for k in (headers or {})):
            req.add_header("Content-Type", "application/json")
    try:
        with _OPENER.open(req, timeout=timeout) as resp:
            raw = resp.read()
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


def header_get(headers: dict[str, str], name: str) -> str | None:
    for k, v in headers.items():
        if k.lower() == name.lower():
            return v
    return None


def curl_equiv(
    method: str, base: str, path: str, headers: dict, body: str | None
) -> str:
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


# --------------------------------------------------------------------------
# FastAPI reference process lifecycle
# --------------------------------------------------------------------------


def start_fastapi(base: str, python_exe: str) -> subprocess.Popen:
    parsed = urllib.parse.urlsplit(base)
    port = parsed.port or 18771
    host = parsed.hostname or "127.0.0.1"
    return subprocess.Popen(
        [
            python_exe,
            "-m",
            "uvicorn",
            "fastapi_app:app",
            "--host",
            host,
            "--port",
            str(port),
            "--log-level",
            "warning",
        ],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def wait_ready(base: str, proc: subprocess.Popen | None, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc is not None and proc.poll() is not None:
            return False
        status, _, _ = http_request(base, "GET", "/health", {}, None, timeout=1.0)
        if status == 200:
            return True
        time.sleep(0.15)
    return False


def stop_fastapi(proc: subprocess.Popen | None) -> None:
    if proc is None:
        return
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def _has_fastapi(python_exe: str) -> bool:
    if not python_exe or not Path(python_exe).exists():
        return False
    try:
        r = subprocess.run(
            [python_exe, "-c", "import fastapi, uvicorn"],
            capture_output=True,
            timeout=10,
        )
        return r.returncode == 0
    except Exception:  # noqa: BLE001
        return False


def ensure_reference_python(explicit: str | None) -> str:
    """Return a python executable that can `import fastapi, uvicorn`.

    Deliberately pure Python, not a new shell script: bench/no-shell already
    moved this repo's pipelines out of .sh into .sql + shellfs, and that
    convention is for bench/, not this driver. If the caller pinned an
    interpreter explicitly, honor it exactly (never silently swap it) even
    if the health check below then fails loudly. Otherwise, if the current
    interpreter already has fastapi/uvicorn, use it; failing that, create
    (once) a venv under test/conformance/.venv and pip install
    requirements.txt into it — the one-time setup a human would otherwise
    have to run by hand.
    """
    if explicit:
        return explicit
    if _has_fastapi(sys.executable):
        return sys.executable

    venv_dir = ROOT / ".venv"
    venv_python = venv_dir / (
        "Scripts/python.exe" if os.name == "nt" else "bin/python3"
    )
    if not _has_fastapi(str(venv_python)):
        if not venv_dir.exists():
            subprocess.run([sys.executable, "-m", "venv", str(venv_dir)], check=True)
        subprocess.run(
            [
                str(venv_python),
                "-m",
                "pip",
                "install",
                "--quiet",
                "--disable-pip-version-check",
                "-r",
                str(ROOT / "requirements.txt"),
            ],
            check=True,
        )
    return str(venv_python)


# --------------------------------------------------------------------------
# Differential comparison — the equivalence oracle is the live reference,
# not a hand-encoded expectation.
# --------------------------------------------------------------------------


def types_equivalent(q_type: str, f_type: str) -> bool:
    if q_type == f_type:
        return True
    if q_type in _GENERIC_TYPE_MISMATCH and f_type in _SCALAR_PARSING_TYPES:
        return True
    if f_type in _GENERIC_TYPE_MISMATCH and q_type in _SCALAR_PARSING_TYPES:
        return True
    return False


def parse_set_cookie(value: str) -> dict[str, Any]:
    """Name/value/attrs of one Set-Cookie header, via plain string splits
    on the cookie grammar's own delimiters — never a regex."""
    parts = [p.strip() for p in value.split(";") if p.strip()]
    if not parts:
        return {"name": "", "value": "", "attrs": []}
    name, _, val = parts[0].partition("=")
    return {"name": name, "value": val, "attrs": parts[1:]}


def _location_path(value: str | None) -> str | None:
    if value is None:
        return None
    return urllib.parse.urlsplit(value).path or value


def compare_header(name: str, qv: str | None, fv: str | None) -> list[str]:
    if qv is None and fv is None:
        return []
    lname = name.lower()
    if lname == "content-type":
        qmain = (qv or "").split(";")[0].strip().lower()
        fmain = (fv or "").split(";")[0].strip().lower()
        if qmain != fmain:
            return [f"content-type: quackapi={qv!r} fastapi={fv!r}"]
        return []
    if lname == "allow":
        qset = {m.strip().upper() for m in (qv or "").split(",") if m.strip()}
        fset = {m.strip().upper() for m in (fv or "").split(",") if m.strip()}
        if qset != fset:
            return [f"allow: quackapi={sorted(qset)} fastapi={sorted(fset)}"]
        return []
    if lname == "location":
        if _location_path(qv) != _location_path(fv):
            return [f"location: quackapi={qv!r} fastapi={fv!r}"]
        return []
    if lname == "www-authenticate":
        qscheme = (qv or "").split(" ")[0]
        fscheme = (fv or "").split(" ")[0]
        if qscheme != fscheme:
            return [f"www-authenticate scheme: quackapi={qv!r} fastapi={fv!r}"]
        return []
    if lname == "set-cookie":
        qc = parse_set_cookie(qv) if qv else None
        fc = parse_set_cookie(fv) if fv else None
        if bool(qc) != bool(fc):
            return [f"set-cookie presence: quackapi={qv!r} fastapi={fv!r}"]
        if qc and fc and (qc["name"], qc["value"]) != (fc["name"], fc["value"]):
            return [f"set-cookie name/value: quackapi={qv!r} fastapi={fv!r}"]
        return []
    return []


def compare_validation_bodies(q_json: Any, f_json: Any) -> list[str]:
    """Both sides must be {"detail": [{loc,msg,type}, ...]}. loc is compared
    exactly (named fields); type via the scalar-family table above; msg is
    non-authoritative — quackapi's own wording never matches pydantic's
    verbatim, and requiring it to would fail every validation case forever
    regardless of correctness, which is noise, not signal.
    """
    if not (isinstance(f_json, dict) and isinstance(f_json.get("detail"), list)):
        return [
            f"fastapi reference did not return a {{detail:[...]}} validation body: {f_json!r}"
        ]
    qd = q_json.get("detail") or []
    fd = f_json.get("detail") or []
    failures: list[str] = []
    if len(qd) != len(fd):
        failures.append(f"422 detail length: quackapi={len(qd)} fastapi={len(fd)}")
    for i, (qe, fe) in enumerate(zip(qd, fd)):
        if not (isinstance(qe, dict) and "loc" in qe and "msg" in qe and "type" in qe):
            failures.append(f"422 detail[{i}] missing loc/msg/type: {qe!r}")
            continue
        if qe.get("loc") != fe.get("loc"):
            failures.append(
                f"422 detail[{i}] loc: quackapi={qe.get('loc')!r} fastapi={fe.get('loc')!r}"
            )
        if not types_equivalent(str(qe.get("type")), str(fe.get("type"))):
            failures.append(
                f"422 detail[{i}] type: quackapi={qe.get('type')!r} fastapi={fe.get('type')!r}"
            )
    return failures


def compare_body(case: dict, q: dict, f: dict) -> list[str]:
    if case.get("expect_body_empty"):
        failures = []
        if q["body"]:
            failures.append(f"quackapi body not empty: {q['body'][:80]!r}")
        if f["body"]:
            failures.append(f"fastapi body not empty: {f['body'][:80]!r}")
        return failures

    markers = case.get("expect_body_contains") or []

    if case.get("id") in MARKER_ONLY_CASE_IDS:
        # OpenAPI/Swagger/ReDoc bodies genuinely differ in full: quackapi
        # generates its own spec/HTML, not a copy of FastAPI's. Marker
        # presence on BOTH sides is the meaningful, named check here.
        qtext = q["body"].decode("utf-8", errors="replace")
        ftext = f["body"].decode("utf-8", errors="replace")
        failures = []
        for m in markers:
            if m not in qtext:
                failures.append(f"quackapi body missing marker {m!r}")
            if m not in ftext:
                failures.append(
                    f"fastapi reference body missing marker {m!r} (marker itself may be stale)"
                )
        return failures

    qct = header_get(q["headers"], "Content-Type") or ""
    fct = header_get(f["headers"], "Content-Type") or ""
    q_json = parse_json(q["body"]) if "json" in qct.lower() else None
    f_json = parse_json(f["body"]) if "json" in fct.lower() else None

    if (
        q["status"] >= 400
        and isinstance(q_json, dict)
        and isinstance(q_json.get("detail"), list)
    ):
        return compare_validation_bodies(q_json, f_json)

    if q_json is not None or f_json is not None:
        if q_json != f_json:
            return [f"json body mismatch: quackapi={q_json!r} fastapi={f_json!r}"]
        return []

    qtext = q["body"].decode("utf-8", errors="replace")
    ftext = f["body"].decode("utf-8", errors="replace")
    failures = []
    for m in markers:
        if m not in qtext:
            failures.append(f"quackapi body missing {m!r}: {qtext[:200]!r}")
        if m not in ftext:
            failures.append(
                f"fastapi reference body missing {m!r} (marker itself may be stale): {ftext[:200]!r}"
            )
    return failures


def extra_assertions(case: dict, q: dict) -> list[str]:
    """Named, per-case checks beyond live-diff, for values the differential
    layer alone wouldn't pin down as precisely."""
    failures: list[str] = []
    if case.get("id") == "set_cookie":
        raw = header_get(q["headers"], "Set-Cookie")
        if not raw:
            failures.append("quackapi: missing Set-Cookie header")
        else:
            c = parse_set_cookie(raw)
            if c["name"] != "session" or c["value"] != "sess-abc":
                failures.append(
                    f"quackapi cookie name/value: name={c['name']!r} value={c['value']!r}"
                )
            if "Path=/" not in c["attrs"]:
                failures.append(
                    f"quackapi cookie missing Path=/ attribute: attrs={c['attrs']!r}"
                )
    return failures


def legacy_pinned_assertions(case: dict, q: dict) -> list[str]:
    """Literal, human-set value pins against quackapi alone.

    These predate the live differential layer and are redundant with it on
    a healthy run (full-body JSON equality against a live reference already
    implies these), but they're defense in depth for the one thing a pure
    differential can't catch: the reference and quackapi being wrong in the
    same way. Kept deliberately narrow — named field/header lookups only,
    never a positional path walker.
    """
    failures: list[str] = []
    j = parse_json(q["body"])
    ct = header_get(q["headers"], "Content-Type") or ""

    if case.get("expect_ct_substr") and case["expect_ct_substr"] not in ct:
        failures.append(
            f"content-type: observed={ct!r} expected substr={case['expect_ct_substr']!r}"
        )

    if (
        case.get("expect_header_present")
        and header_get(q["headers"], case["expect_header_present"]) is None
    ):
        failures.append(f"missing header {case['expect_header_present']}")

    for hk, hv in (case.get("expect_header_eq") or {}).items():
        got = header_get(q["headers"], hk)
        if got != hv:
            failures.append(f"header {hk}: observed={got!r} expected={hv!r}")

    if "expect_body_json" in case and j != case["expect_body_json"]:
        failures.append(
            f"json body: observed={j!r} expected={case['expect_body_json']!r}"
        )

    if case.get("expect_json_len") is not None:
        if not isinstance(j, list) or len(j) != case["expect_json_len"]:
            failures.append(
                f"json len: observed={j!r} expected list len={case['expect_json_len']}"
            )

    e422 = case.get("expect_422")
    if (
        e422
        and isinstance(j, dict)
        and isinstance(j.get("detail"), list)
        and j["detail"]
    ):
        d0 = j["detail"][0]
        if isinstance(d0, dict):
            loc = d0.get("loc") or []
            if e422.get("loc0") and (not loc or loc[0] != e422["loc0"]):
                failures.append(f"422 loc[0]: observed={loc} expected {e422['loc0']}")
            if e422.get("loc1") and (len(loc) < 2 or loc[1] != e422["loc1"]):
                failures.append(f"422 loc[1]: observed={loc} expected {e422['loc1']}")

    if (
        case.get("expect_422_keys")
        and isinstance(j, dict)
        and isinstance(j.get("detail"), list)
        and j["detail"]
    ):
        d0 = j["detail"][0]
        for k in case["expect_422_keys"]:
            if isinstance(d0, dict) and k not in d0:
                failures.append(f"422 missing key {k}")

    return failures


def evaluate(case: dict, q: dict, f: dict) -> tuple[str, str, list[str]]:
    """q/f: {"status": int, "headers": Dict[str,str], "body": bytes}."""
    notes: list[str] = []
    if case.get("notes"):
        notes.append(case["notes"])

    if case.get("force_na") or case.get("skip_run"):
        return "N/A", case.get("notes") or "not built", []

    if q["status"] == 0:
        return (
            "FAIL",
            "quackapi request failed",
            [q["body"].decode("utf-8", errors="replace")],
        )
    if f["status"] == 0:
        return (
            "FAIL",
            "FastAPI reference request failed",
            [f["body"].decode("utf-8", errors="replace")],
        )

    failures: list[str] = []

    if case.get("expect_status") is not None and q["status"] != case["expect_status"]:
        failures.append(
            f"quackapi status: observed={q['status']} expected={case['expect_status']}"
        )

    if q["status"] != f["status"]:
        failures.append(
            f"status divergence: quackapi={q['status']} fastapi={f['status']}"
        )

    for name in COMPARABLE_HEADERS:
        qv = header_get(q["headers"], name)
        fv = header_get(f["headers"], name)
        failures.extend(compare_header(name, qv, fv))

    failures.extend(compare_body(case, q, f))
    failures.extend(extra_assertions(case, q))
    failures.extend(legacy_pinned_assertions(case, q))

    if failures:
        return "FAIL", "; ".join(notes + failures), failures
    return "PASS", "; ".join(notes) if notes else "match", []


def classify(case: dict, verdict: str) -> str:
    if verdict == "N/A":
        return case.get("class_hint") or "NOT-BUILT-YET"
    if verdict == "PASS":
        return "STRONGER" if case.get("class_hint") == "STRONGER" else "MATCH"
    hint = case.get("class_hint")
    if hint in FAIL_CLASSES:
        return hint
    return "BUG"


def _slim(resp: dict) -> dict:
    if resp.get("status") is None:
        return {"status": None, "headers": {}, "body": None}
    headers = {
        k: v
        for k, v in resp["headers"].items()
        if k.lower() in COMPARABLE_HEADERS or k.lower() == "content-length"
    }
    return {
        "status": resp["status"],
        "headers": headers,
        "body": resp["body"].decode("utf-8", errors="replace")[:500],
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--base", default=os.environ.get("QUACKAPI_BASE", "http://127.0.0.1:18770")
    )
    ap.add_argument(
        "--fastapi-base",
        default=os.environ.get("QUACKAPI_FASTAPI_BASE", "http://127.0.0.1:18771"),
    )
    ap.add_argument(
        "--fastapi-python",
        default=os.environ.get("QUACKAPI_FASTAPI_PYTHON"),
        help="Interpreter with fastapi/uvicorn installed. Left unset, one is auto-provisioned (see ensure_reference_python).",
    )
    ap.add_argument(
        "--no-start-fastapi",
        action="store_true",
        default=os.environ.get("QUACKAPI_NO_START_FASTAPI") == "1",
        help="Assume --fastapi-base is already serving; never spawn our own uvicorn.",
    )
    ap.add_argument("--cases", default=str(CASES_PATH))
    ap.add_argument("--out", default=str(RESULTS_PATH))
    args = ap.parse_args()

    cases_bytes = Path(args.cases).read_bytes()
    cases_sha256 = hashlib.sha256(cases_bytes).hexdigest()
    cases = [
        json.loads(line)
        for line in cases_bytes.decode("utf-8").splitlines()
        if line.strip()
    ]

    fastapi_proc: subprocess.Popen | None = None
    if not args.no_start_fastapi:
        args.fastapi_python = ensure_reference_python(args.fastapi_python)
        fastapi_proc = start_fastapi(args.fastapi_base, args.fastapi_python)

    try:
        if not wait_ready(args.fastapi_base, fastapi_proc, timeout=20.0):
            print(
                f"FATAL: FastAPI reference at {args.fastapi_base} never became reachable.",
                file=sys.stderr,
            )
            print(
                "Conformance requires a live reference for every case; refusing to report equivalence without one.",
                file=sys.stderr,
            )
            if fastapi_proc is not None and fastapi_proc.stdout is not None:
                try:
                    tail = fastapi_proc.stdout.read(4000)
                except Exception:  # noqa: BLE001
                    tail = b""
                if tail:
                    print("--- fastapi_app.py output ---", file=sys.stderr)
                    print(tail.decode("utf-8", errors="replace"), file=sys.stderr)
            return 3

        if not wait_ready(args.base, None, timeout=5.0):
            print(f"FATAL: quackapi is not reachable at {args.base}.", file=sys.stderr)
            return 3

        jwt_token = make_jwt()

        results: list[dict] = []
        counts: dict[str, int] = {"PASS": 0, "FAIL": 0, "N/A": 0}
        classes: dict[str, int] = {}
        groups: dict[str, dict[str, int]] = {}

        for case in cases:
            cid = case["id"]
            group = case.get("group", "misc")
            groups.setdefault(group, {"PASS": 0, "FAIL": 0, "N/A": 0, "total": 0})
            groups[group]["total"] += 1

            if case.get("force_na") or case.get("skip_run"):
                verdict, notes, failures = "N/A", case.get("notes") or "not built", []
                q = {"status": None, "headers": {}, "body": b""}
                f = {"status": None, "headers": {}, "body": b""}
            else:
                headers = dict(case.get("headers") or {})
                if case.get("jwt"):
                    headers["Authorization"] = f"Bearer {jwt_token}"
                qs, qh, qb = http_request(
                    args.base, case["method"], case["path"], headers, case.get("body")
                )
                q = {"status": qs, "headers": qh, "body": qb}
                fs, fh, fb = http_request(
                    args.fastapi_base,
                    case["method"],
                    case["path"],
                    headers,
                    case.get("body"),
                )
                f = {"status": fs, "headers": fh, "body": fb}

                if fs == 0:
                    print(
                        f"FATAL: FastAPI reference stopped responding mid-run at case {cid!r}.",
                        file=sys.stderr,
                    )
                    print(
                        "Aborting without writing results; a partial run must never read as a completed one.",
                        file=sys.stderr,
                    )
                    return 4

                verdict, notes, failures = evaluate(case, q, f)

            cls = classify(case, verdict)
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
                "curl_quackapi": curl_equiv(
                    case["method"],
                    args.base,
                    case["path"],
                    case.get("headers") or {},
                    case.get("body"),
                ),
                "fastapi_doc": case.get("fastapi_doc"),
                "observed_quackapi": _slim(q),
                "observed_fastapi": _slim(f),
                "failures": failures,
                "notes": notes,
            }
            results.append(row)
            mark = {"PASS": "✓", "FAIL": "✗", "N/A": "·"}[verdict]
            print(
                f"{mark} {cid:32} {verdict:4} {cls:14} q={q.get('status')} f={f.get('status')} {notes[:80]}"
            )

        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        with open(out, "w") as fh_out:
            for r in results:
                fh_out.write(json.dumps(r, ensure_ascii=False) + "\n")

        summary = {
            "run_id": str(uuid.uuid4()),
            "started_at": datetime.now(UTC).isoformat(),
            "cases_sha256": cases_sha256,
            "case_count": len(cases),
            "quackapi_base": args.base,
            "fastapi_base": args.fastapi_base,
            "fastapi_version": FASTAPI_PINNED_VERSION,
            "reference_reachable": True,
            "total": len(results),
            "counts": counts,
            "classes": classes,
            "groups": groups,
        }
        summary_path = out.parent / "summary.json"
        with open(summary_path, "w") as fh_sum:
            json.dump(summary, fh_sum, indent=2)

        print("\n=== SUMMARY ===")
        print(json.dumps(summary, indent=2))
        print(f"wrote {out}")
        return 0 if counts.get("FAIL", 0) == 0 else 1
    finally:
        stop_fastapi(fastapi_proc)


if __name__ == "__main__":
    sys.exit(main())
