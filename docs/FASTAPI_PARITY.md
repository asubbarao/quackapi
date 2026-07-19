# quackapi ↔ FastAPI parity scorecard

**Source of truth:** [fastapi.tiangolo.com](https://fastapi.tiangolo.com/)  
**Harness (versioned):** `test/conformance/`  
**Method:** every PASS/FAIL is a **real HTTP request** against `quackapi_serve()` (FIFO interactive session — never `duckdb -c` for live serve).  
**Date:** 2026-07-19 (G4 final verification)

---

## Headline (final)

| Metric | Before G4 | After G4 |
|--------|----------:|---------:|
| **Overall** | 86 / 89 (96.6%) | **89 / 89 (100%)** |
| Runnable (excl. N/A) | 86 / 87 | **89 / 89** |
| **BUG** | 0 | **0** |
| FAIL | 1 (`health_options`) | **0** |
| N/A | 2 (form/multipart marked unbuilt) | **0** |
| STRONGER (class) | 1 | **1** (+ documented stronger behaviors below) |

```
PASS 89   FAIL 0   N/A 0   total 89
classes: MATCH 88, STRONGER 1, BUG 0
```

### Conformance % by group (final)

| Group | Match | Total | % |
|-------|------:|------:|--:|
| auth | 6 | 6 | **100%** |
| content_types | 7 | 7 | **100%** |
| methods | 9 | 9 | **100%** |
| openapi | 3 | 3 | **100%** |
| params | 26 | 26 | **100%** |
| routing | 12 | 12 | **100%** |
| status_codes | 5 | 5 | **100%** |
| validation | 21 | 21 | **100%** |

---

## How to re-run

```bash
CMAKE_BUILD_PARALLEL_LEVEL=4 MAKEFLAGS=-j4 make release
bash test/conformance/run.sh
python3 test/conformance/render_scorecard.py
bash test/http/run_all.sh
```

---

## G4 fixes that closed the last gaps

1. **OPTIONS without CORS → 405 + Allow**  
   FastAPI/Starlette without `CORSMiddleware` rejects unregistered OPTIONS with 405.  
   When `quackapi_cors_origins` / `cors_origins` is set, OPTIONS still returns **204** preflight + CORS headers (stronger discovery when enabled).

2. **Form + multipart conformance cases enabled**  
   Features were already shipped (HTTP tests green); fixture routes + cases were still `force_na`. Now live PASS.

---

## STRONGER than FastAPI

| Behavior | Why stronger |
|----------|----------------|
| Response JSON follows **column types** (bool / number / null / nested) | No stringly JSON; DB is the type system |
| `GET /users/<int64-overflow>` → **422** | FastAPI accepts arbitrary Python int then application “not found”; quackapi fails closed |
| Handlers are set-based SQL | Validation + projection in one query; no ORM/Pydantic dual schema |
| `SELECT … AS html` / `AS text` / `AS location` / `AS set_cookie` | Zero-framework custom response via column name |
| `SELECT` column list as response model | FastAPI `response_model` exclude/include is app-side; SQL projection is native |
| Strict integer bind (reject `1.5`, `1e2`) | Matches Pydantic v2 int parsing; no silent cast-rounding |

---

## INTENTIONAL divergences (still PASS on field semantics)

| Divergence | quackapi | FastAPI | Justification |
|------------|----------|---------|---------------|
| **Response envelope** | Always JSON **array of row objects** (`[{"status":"ok"}]`) | Often a bare object for a single dict | SQL result set semantics; scorecard matches on fields + status |
| **Query vs body loc** | Missing query-bound fields use `loc=["query", name]` when the client did not send a JSON body model | Body models use `loc=["body", name]` | Surface is SQL `$param` binding; JSON body still uses `loc=["body", …]` when fields come from the body |
| **CORS default** | CORS **off** until `SET quackapi_cors_origins` / serve `cors_origins` | Same — CORS is middleware opt-in | Documented; OPTIONS 405 without CORS matches FastAPI |

No silent gaps: anything not MATCH is either **STRONGER** or **INTENTIONAL** as above.

---

## HTTP test coverage map (shipped features)

| Feature | Test |
|---------|------|
| 5-liner + path 422 | `test/http/fiveliner.test.sh` |
| Strict int / optional / LE | `test/http/validation.test.sh` |
| JSON body + 422 shapes | `test/http/body.test.sh` |
| BODY SCHEMA | `test/http/body_schema.test.sh` |
| Form / multipart | `test/http/form.test.sh`, `multipart.test.sh` |
| HEADER / COOKIE / Set-Cookie | `test/http/headers.test.sh`, `cookies.test.sh` |
| Redirect | `test/http/redirect.test.sh` |
| Trailing slash 307 | `test/http/trailing_slash.test.sh` |
| Auth API key + JWT | `test/http/auth.test.sh` |
| 404 / 405+Allow / OPTIONS / HEAD | `test/http/routing.test.sh` |
| CORS preflight | `test/http/cors.test.sh` |
| openapi / docs / redoc | `test/http/redoc.test.sh` |

`bash test/http/run_all.sh` must stay green.

---

## Classification key

| Class | Meaning |
|-------|---------|
| **MATCH** | Observed status + core body/header semantics match FastAPI docs for that behavior |
| **STRONGER** | Exceeds FastAPI safety or type fidelity on the same surface |
| **INTENTIONAL** | Documented design divergence (envelope, bind location when using query surface) |
| **BUG** | Should match and does not — **must be 0 for release** |
