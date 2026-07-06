# quackapi Test Plan

**Generated:** 2026-07-02  
**Entry point:** `bash test/run_all.sh`  
**With C++ suites:** `bash test/run_all.sh --with-cpp`

---

## Suite → Layer → Feature-Gap-Matrix Coverage

| Suite | File(s) | Layer | Checks (last run) | What it tests | FEATURE_GAP_MATRIX rows covered |
|-------|---------|-------|-------------------|---------------|----------------------------------|
| `tier1:handle_request` | `test/tier1_handle_request.test.sql` | SQL oracle | 112 | `handle_request()` routing, path params, query params, body JSON, 422 shape, response headers, cookies, form params (header/cookie/form location=), redirects (307+Location), Set-Cookie via route_headers, SSE route, method-mismatch 405, HEAD as GET, CORS pre/post, openapi.json, /docs | §1 Routing (all rows), §2 Request handling (path/query/body/headers/cookies), §3 Validation (types, constraints, optional, 422 shape), §4 Responses (status codes, headers, SSE, redirect), §8 Middleware (CORS, custom headers), §11 OpenAPI |
| `di:dependency_injection` | `test/di.test.sql` | SQL oracle | 8 | `di_resolve()` + `di_teardown()`: token-based DI, request-scoped context JSON, per-request lifecycle (resolve→handler→teardown) | §6 Dependency injection (Depends, yield-DI, request-scoped state) |
| `middleware:chain` | `test/middleware.test.sql` | SQL oracle | 12 | `apply_pre()` / `apply_post()` middleware chain: auth gate (Bearer), CORS preflight short-circuit (OPTIONS 204 + ACA headers), response header injection (X-Powered-By, X-Frame-Options), background enqueue stub (`dispatch_async`), end-to-end pre→route→post | §8 Middleware (CORS, custom middleware, exception handlers), §9 Background (BackgroundTasks) |
| `tier2:http_wire` | `test/tier2_http.sh` | HTTP wire | 14 (when server up) | Live HTTP against the running server: GET /users/1, GET /users/abc (422), GET /users, GET /openapi.json (version+title+paths), GET /docs (DOCTYPE+swagger-ui), GET /nope (404), GET /search?limit=999 (422 less_than_equal); optional POST /users | §1 Routing (real HTTP), §2 Request (JSON, wire), §3 Validation (422 wire), §11 OpenAPI/docs |
| `ext-cpp:sqllogictest` | `ext-cpp/test/sql/*.test` | C++ extension | ~3 test files | `quack_route_decision()` via sqllogictest: routing logic (`quackapi_routing.test`), error paths (`quackapi_errors.test`), CREATE/DROP ROUTE DDL (`quackapi_route_ddl.test`) | §1 Routing (C router), §3 Validation (C layer), §1 sub-row "CREATE ROUTE DDL" |
| `ext-cpp:parity_b2` | `ext-cpp/parity_b2.sh` | C++ parity | ~42 cases | Byte/semantic identity between `handle_request()` SQL oracle and `quack_route_decision()` C router across: path params, 422 types, query params, body POST 201, 404/405, header/cookie/form params (R1), redirect+Set-Cookie, HEAD, method-mismatch, DELETE 405+Allow, CORS (R1), multipart (R2) | §1 Routing (parity), §2 Request (all param locations), §3 Validation (all types), §4 Responses (headers, redirects), §8 CORS |
| `conformance-pure:sql_vs_fastapi` | `test/conformance/run_conformance_pure.sh` + `driver_pure.py` + `cases.jsonl` | Differential (SQL vs FastAPI HTTP) | 87 | Pure-SQL `handle_request()` vs real FastAPI+uvicorn HTTP: 87 cases from `cases.jsonl` replayed against both | §13 Testing story (differential); exposes §1 HEAD auto-handling (HARD), §2 float body coercion (BUG), §3 type coercion edge cases, §5 405 Allow header differences |
| `conformance:differential` | `test/conformance/run_conformance.sh` + `driver.py` | Differential (C ext vs FastAPI HTTP) | Requires built C ext + Python | Same 87 cases through the running C extension server vs FastAPI | Same as conformance-pure plus C layer specifically |
| `fuzz:oracle_property` | `test/fuzz/oracle_fuzz.test.sql` | SQL oracle (property) | 100 | 7 property sections: router ambiguity/precedence, integer constraint boundaries, body edge cases (null/float/unicode), 422 shape invariants, idempotence, method/header variants (HEAD/405/DELETE Allow), JSON-null vs absent key boundary | §1 Routing (edge: double-slash, trailing slash, literal precedence, pct-encoding), §3 Validation (boundaries, edge cases), §4 Responses (405 Allow), §3 body edge cases |

---

## Coverage Gaps — Features in FEATURE_GAP_MATRIX with No Test

The following rows in `docs/FEATURE_GAP_MATRIX.md` are **not covered** by any current suite:

| Feature | Matrix status | Gap type | Notes |
|---------|---------------|----------|-------|
| Sub-routers / mounts (APIRouter) | NATURAL | No test | No prefix column or filtering implementation to test |
| WebSocket routes | HARD | No test | `serve_ws.sql` exists but is not wired into main server path; no test covers ws:// route |
| Multipart file upload (large) | HARD | Partial only | Parity harness covers small multipart; no test for 64 KB ceiling or boundary-parse path |
| TLS termination | HARD | No test | No HTTPS path anywhere in codebase |
| Async/await handlers | HARD | No test | No async runtime; worker model is 16 blocking pthreads |
| True yield-DI (resource lifetime) | PARTIAL | No integration test | `di.sql` teardown tested in isolation; no test that a crash between send and teardown leaves teardown un-run |
| Nested Pydantic models | HARD | No test | Flat param_schema only; no nested body validation exists to test |
| Custom validators (@validator) | NATURAL | No test | Pattern documented but no test exercises it |
| `response_model` filtering | N/A | No test | Intentional; handler SQL decides projection |
| Content-type negotiation (`Accept`) | N/A (partial) | No test | No `Accept` parsing in router |
| OAuth2 / JWT helpers | N/A | No test | No JWT lib in repo |
| Multiple-process model | NATURAL | No test | Process model note only; no multi-process CI |
| Graceful shutdown | HARD | No test | No shutdown flag or drain |
| Logging / observability hooks | PRESENT-UNTESTED | No test | `request_logger` middleware example only |
| OpenAPI `securitySchemes` | HARD | No test | OpenAPI builder emits only paths+params |
| OpenAPI `components` depth | PRESENT-UNTESTED | No test | Only paths+parameters+422 response emitted |
| GZip middleware | HARD | No test | No compression in response path |
| Startup/shutdown lifespan | HARD | No test | No lifespan hook exists |
| ReDoc | N/A | No test | Only Swagger UI |
| Sub-router prefix/mounts | NATURAL | No test | Flat routes table; no prefix implementation |
| Trailing-slash behavior | PRESENT-UNTESTED | Partial | fuzz suite covers /users/ and /users/1/ but no specific slash-redirect test |
| `Set-Cookie` response helper | NATURAL | Indirect only | Covered indirectly via tier1 R1 login route; no dedicated cookie helper test |
| `FileResponse` / `RedirectResponse` | WON (subset) | Partial | Redirect covered; FileResponse not tested (no fs serve) |
| Background task durability | WON | No durability test | Dispatch tested; no test that task fires after simulated crash |

---

## Known Active Failures (honest as of 2026-07-02)

### `fuzz:oracle_property` — 3 failures (documented bugs, not regressions)

These are intentional pass=false oracle entries that document known bugs:

| Check name | Bug |
|------------|-----|
| `BODY age=1.5 (float in JSON) → 422 int_parsing` | `try_cast(1.5, INTEGER)` succeeds → float accepted where int required; FastAPI rejects. |
| `BODY malformed JSON — BUG: throws not 422` | `json_extract_string` on malformed body throws `Invalid Input Error` instead of returning a 422 response. |
| `BODY whitespace-only — BUG: throws not 422` | Whitespace body bypasses NULL guard; `json_extract_string` throws. |
| `ROUTER duplicate query key — BUG: throws` | `map_from_entries` in `query_map` CTE throws `Map keys must be unique` on `?q=a&q=b`. |

(Note: run count varies 3–8 depending on whether `ROUTER lowercase method get` is included in the run; the pure SQL oracle counts `get` → `405` as passing in some runs.)

### `conformance-pure:sql_vs_fastapi` — ~40 failures (documented feature gaps)

These are real behavioral divergences between quackapi's SQL oracle and real FastAPI:

- **HEAD requests** (8 cases): FastAPI auto-handles HEAD from GET routes (405); quackapi returns 200 (treats HEAD as GET). Gap documented in §1 of matrix as HARD.
- **Float coercion** (multiple): FastAPI rejects `age=1.5` (int field) with 422; quackapi's `try_cast` accepts it → 200. Documented as a known validation edge case.
- **405 Allow header content** (multiple): FastAPI emits `Allow: GET, HEAD, OPTIONS`; quackapi emits `Allow: GET, HEAD` (no OPTIONS). Minor difference.
- **Various other schema edge cases**: Float in query param for int field, overflow values, etc.

These divergences are coverage data, not blockers — they represent the feature gap the project is actively closing.

---

## How to Run Everything

```bash
# Fast pure-SQL suites (no server, no build — under 30s):
bash test/run_all.sh

# With C++ extension suites (requires prior build — do NOT trigger build):
bash test/run_all.sh --with-cpp

# Build the C++ extension first (separate step, sibling agent owns this):
cd ext-cpp && make release && cd ..
bash test/run_all.sh --with-cpp

# Tier-2 HTTP tests (requires a running server):
printf '.read framework.sql\n.read app.sql\n' \
  | /opt/homebrew/bin/duckdb -unsigned quackapi.db
# Then in another terminal:
bash test/tier2_http.sh

# Individual suites:
printf '.read framework.sql\n.read test/tier1_handle_request.test.sql\n' \
  | /opt/homebrew/bin/duckdb -unsigned          # tier1

printf '.read framework.sql\n.read di.sql\n.read test/di.test.sql\n' \
  | /opt/homebrew/bin/duckdb -unsigned          # di

printf '.read framework.sql\n.read middleware.sql\n.read test/middleware.test.sql\n' \
  | /opt/homebrew/bin/duckdb -unsigned          # middleware

bash test/fuzz/run_oracle_fuzz.sh              # fuzz (wrapper; exits 2 if parse fails)
# Or directly:
printf '.read framework.sql\n.read app.sql\n.read test/fuzz/oracle_fuzz.test.sql\n' \
  | /opt/homebrew/bin/duckdb -unsigned          # fuzz (direct)

bash test/conformance/run_conformance_pure.sh  # conformance (pure-SQL vs FastAPI)
```

### CI summary

- **Job `sql-oracle`** (fast, required): tier1 + di + middleware + fuzz. Runs in ~30s on ubuntu-latest.
- **Job `conformance-pure`** (allow-failure): pure-SQL oracle vs FastAPI reference. Requires Python + pip; boots uvicorn.
- **Job `cpp-extension`** (allow-failure): full C++ build (vcpkg+ninja) + `make test` + `parity_b2.sh`. Heavy — ~5–15 min. Not a merge blocker until build is CI-hardened.
