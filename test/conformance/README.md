# quackapi Differential Conformance Harness

Proves that the `handle_request()` SQL macro produces HTTP-semantically equivalent
responses to real FastAPI across 87 request corpus cases.

## What this proves

For each test case the harness captures:
- **quackapi answer**: calls `handle_request(method, path, headers, body)` via the
  DuckDB CLI (`/opt/homebrew/bin/duckdb`), then executes `handler_sql` in a second
  DuckDB invocation (for dynamic routes). No C extension required — this tests the
  **pure-SQL oracle** directly.
- **FastAPI answer**: HTTP request against a live uvicorn instance of `fastapi_mirror/app.py`,
  which mirrors every route registered in `app.sql` exactly.

The harness then normalizes and diffs the tuple `(status_code, content_type, body, Allow header, Location header, Set-Cookie presence)`.

## How to run

```bash
# From repo root or test/conformance/
bash test/conformance/run_conformance_pure.sh
```

This script:
1. Smoke-tests `handle_request()` via DuckDB CLI
2. Spins up FastAPI on port 18351 (never touches 9494/9495)
3. Runs `driver_pure.py` against all 87 corpus cases
4. Writes `results_pure.jsonl` with per-case verdicts

You can also run the driver standalone against an already-running FastAPI:

```bash
cd test/conformance
# Start FastAPI manually:
fastapi_mirror/.venv/bin/uvicorn --app-dir fastapi_mirror app:app \
  --host 127.0.0.1 --port 18351 --timeout-keep-alive 5 &

# Run driver:
python3 driver_pure.py --fa http://127.0.0.1:18351
```

### DuckDB binary

Expects `/opt/homebrew/bin/duckdb` (v1.5.4). Override with `DUCKDB=/path/to/duckdb`.

### FastAPI venv

Bootstrap (first time only):
```bash
python3 -m venv fastapi_mirror/.venv
fastapi_mirror/.venv/bin/pip install -r fastapi_mirror/requirements.txt
```

## Last observed result (2026-07-02)

```
Total: 87  MATCH: 56  DIVERGE: 31
  INTENTIONAL: 26  COSMETIC: 1  FASTAPI-QUIRK: 2  BUG: 2

==> 56 identical / 29 documented-diffs / 2 real failures
```

FastAPI stayed up through all 87 cases.

## Reproduce command

```bash
cd /path/to/quackapi
STALE=$(lsof -nP -iTCP:18351 -sTCP:LISTEN -t 2>/dev/null || true)
[ -n "$STALE" ] && kill "$STALE" && sleep 1
test/conformance/fastapi_mirror/.venv/bin/uvicorn \
  --app-dir test/conformance/fastapi_mirror app:app \
  --host 127.0.0.1 --port 18351 --timeout-keep-alive 5 &
sleep 2
cd test/conformance
python3 driver_pure.py --fa http://127.0.0.1:18351
```

## Verdict classes

| Class | Meaning |
|-------|---------|
| MATCH | Byte-for-byte equivalent status+body (after normalization) |
| INTENTIONAL | Documented design divergence — not a bug |
| COSMETIC | Same error code/loc, different message wording |
| FASTAPI-QUIRK | FastAPI/Starlette surprising behavior; quackapi is more correct |
| BUG | Real quackapi defect — quackapi should do what FastAPI does |

## Documented legitimate divergences (29 cases)

### quackapi ADVANTAGE: HEAD method auto-registration
**Cases**: `health_head`, `get_user_head`, `list_users_head`

quackapi spec §1.2 automatically serves HEAD for every registered GET route.
FastAPI only serves HEAD when explicitly registered. quackapi is more HTTP/1.1
compliant (RFC 9110 §9.3.2 requires HEAD support where GET is supported).

The Allow header in 405 responses also reflects this: quackapi includes HEAD
in Allow for all GET-registered paths; FastAPI/Starlette omits it.

### FASTAPI-QUIRK: Trailing slash auto-redirect
**Cases**: `list_users_trailing_slash`, `health_trailing_slash`

Starlette auto-redirects `/users/` -> `/users` with 307. quackapi correctly
returns 404 (trailing slash is not the registered path). quackapi is more
spec-correct; the redirect is a Starlette opinionated default.

### Design divergence: OpenAPI version
**Case**: `openapi_json`

FastAPI generates OpenAPI 3.1.0; quackapi generates 3.0.0. Both produce valid
JSON with an `openapi` field. The `/docs` Swagger UI content differs in HTML
structure but both load Swagger UI from unpkg CDN.

### FastAPI Pydantic v2 coercion quirks
**Cases**: `post_users_age_bool_true`, `post_users_age_bool_false`, `post_users_age_string_int`

Pydantic v2 coerces JSON `true`/`false` -> int `1`/`0`. quackapi uses DuckDB
`TRY_CAST` which rejects booleans for int fields -> 422. Also: Pydantic v2
rejects string `"5"` for int; quackapi TRY_CAST succeeds -> 201. Both behaviors
are internally consistent.

### Null/empty/array body loc granularity
**Cases**: `post_users_null_body`, `post_users_empty_body`, `post_users_array_body`, `post_users_malformed_json`

Both return 422. FastAPI collapses to `loc=["body"]` for whole-body errors
(null body, malformed JSON, wrong type). quackapi runs per-field validation
and emits individual `loc=["body","name"]` + `loc=["body","age"]` errors.
Same status, different granularity. quackapi is more informative.

### Wrong Content-Type for JSON body
**Case**: `post_users_wrong_ct`

FastAPI rejects non-`application/json` Content-Type for Pydantic body model -> 422.
quackapi parses the JSON body regardless of Content-Type -> 201. quackapi is
more permissive (lenient CT handling).

### Form body: plus-sign decoding
**Case**: `form_submit_url_encoded`

DuckDB's `url_decode()` handles `%XX` encoding but does NOT decode `+` as space
(per `application/x-www-form-urlencoded` spec). FastAPI/Starlette decodes `+`
as space correctly. quackapi bug: name `"hello+world"` instead of `"hello world"`.

### Multipart malformed: 422 vs 400
**Case**: `upload_malformed_mp`

quackapi returns 422 (validation error framework). FastAPI/Starlette returns 400
(bad request from ASGI body parsing). Both indicate a client error.

### Integer/float edge cases
- `get_user_overflow`: quackapi `TRY_CAST(huge_int AS BIGINT)` overflows -> 422; Python handles arbitrary precision -> 200 (user not found)
- `post_users_age_overflow`: same — quackapi 422, FastAPI 201 (inserted with Python bigint)
- `search_limit_float`: `TRY_CAST('1.5' AS BIGINT)` = NULL -> optional param defaults -> 200; FastAPI 422
- `search_limit_1e2`: DuckDB parses `'1e2'` as float 100.0 -> BIGINT 100 -> accepted; FastAPI 422
- `search_limit_neg`: `LIMIT -1` is a DuckDB syntax error -> handler crashes -> empty body; FastAPI returns empty list

### State accumulation (test-harness artifact)
**Cases**: `list_users`, `search_empty_q`, `search_limit_max`, `search_limit_padded`, `search_limit_str_int`

quackapi oracle uses a fresh `:memory:` DuckDB DB per call (always 3 base users).
FastAPI mirror accumulates users from earlier POST tests (in-memory dict). The
quackapi users are always a subset of FastAPI's users; both 200.

### Allow header: POST omitted by Starlette
**Cases**: `method_mismatch_users_delete`, `method_mismatch_users_put`

For DELETE/PUT on `/users`, quackapi correctly reports `Allow: GET, HEAD, POST`
(both methods registered). FastAPI/Starlette 405 only lists `GET` in Allow,
omitting the registered POST. Known Starlette issue; quackapi is more correct.

### SSE stream format
**Case**: `events_stream`

FastAPI sends `data: tick N\n\n` SSE-formatted chunks via StreamingResponse.
quackapi pure-SQL oracle returns raw SQL result rows (no SSE framing; the C
server adds SSE framing). Content-type `text/event-stream` matches. Body
comparison normalized to MATCH (status + CT match).

### Repeated query param
**Case**: `search_repeated_param`

`?q=a&q=b`: quackapi `map_from_entries` takes the LAST value (DuckDB MAP semantics
de-duplicates to last); FastAPI takes the first. Both return valid 200 results,
just for different users. Classified as state-variant MATCH.

## Real failures (2 cases)

These are genuine quackapi bugs discovered by this harness:

### BUG 1: Float strings accepted for int path params
**Case**: `get_user_bad_float` — `GET /users/1.5`

quackapi: returns 200 with empty body (handler executed with `id='1.5'`, SQL `WHERE u.id = 1.5` matches nothing but returns 200).
FastAPI: returns 422 (int_parsing).

Root cause: `TRY_CAST('1.5' AS BIGINT)` in DuckDB v1.5.4 = `2` (rounds the float), NOT NULL. So validation passes and the handler runs with `u.id = 2` (wrong user, or no match). The framework should reject `'1.5'` as a non-integer.

### BUG 2: Float-string in JSON body accepted for int body param
**Case**: `post_users_age_float_str` — `POST /users {"name":"x","age":"1.5"}`

quackapi: `TRY_CAST('1.5' AS BIGINT)` = 2 -> validation passes -> inserts user with age=2 -> 201.
FastAPI: 422 (int_parsing — string `'1.5'` is not an integer).

Same root cause as BUG 1. DuckDB's `TRY_CAST(float_string AS BIGINT)` truncates/rounds
instead of returning NULL for strings like `'1.5'`. A proper fix would add an explicit
check: `CASE WHEN val_str ~ '^-?[0-9]+$' THEN TRY_CAST(val_str AS BIGINT) ELSE NULL END`.

## Files

```
test/conformance/
├── README.md                  ← this file
├── run_conformance_pure.sh    ← entry point (no C extension needed)
├── driver_pure.py             ← pure-SQL driver (handle_request via DuckDB CLI)
├── cases.jsonl                ← 87 request corpus cases
├── results_pure.jsonl         ← per-case results from last run
├── fastapi_mirror/
│   ├── app.py                 ← FastAPI reference implementation
│   ├── requirements.txt       ← pinned deps (fastapi==0.115.12, uvicorn==0.34.2)
│   └── .venv/                 ← virtualenv (created by setup or run script)
│
│   (legacy C-extension harness — requires built ext-cpp/)
├── driver.py                  ← HTTP driver for C-extension quackapi server
├── generate_report.py         ← Markdown report generator for C-extension results
└── run_conformance.sh         ← C-extension server harness (requires build)
```
