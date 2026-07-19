# quackapi tests

## Prerequisites

```bash
CMAKE_BUILD_PARALLEL_LEVEL=4 MAKEFLAGS=-j4 make release
```

Binary: `./build/release/duckdb` (use `-unsigned` when loading a local extension build).

**FIFO / interactive stdin:** `duckdb -c "a;b"` parses all statements before executing, so
parser-extension DDL after `LOAD` and a live `quackapi_serve()` must be fed one-at-a-time
via a FIFO or interactive session. All HTTP and conformance harnesses do this for you.

## HTTP integration tests (mandatory for shipped HTTP features)

Real `quackapi_serve` + `curl` against a high port; each case stops with `quackapi_stop()`.

```bash
bash test/http/run_all.sh
```

Individual cases:

```bash
bash test/http/fiveliner.test.sh
bash test/http/validation.test.sh
bash test/http/body.test.sh
# …see test/http/*.test.sh
```

| File | Coverage |
|------|----------|
| `fiveliner.test.sh` | README 5-liner gate + path 422 shape |
| `validation.test.sh` | strict int, optional `PARAM DEFAULT`, `LE`, never-500 |
| `body.test.sh` | JSON body binder, malformed / wrong CT |
| `body_schema.test.sh` | `BODY SCHEMA` JSON Schema validation |
| `form.test.sh` | `application/x-www-form-urlencoded` |
| `multipart.test.sh` | multipart file + fields |
| `headers.test.sh` | `PARAM … HEADER` |
| `cookies.test.sh` | `PARAM … COOKIE` + `set_cookie` column |
| `redirect.test.sh` | 3xx + `location` column |
| `trailing_slash.test.sh` | Starlette-style 307 trailing slash |
| `redoc.test.sh` | `/openapi.json`, `/docs`, `/redoc` |
| `auth.test.sh` | API_KEY + JWT `REQUIRE` → 401 |
| `routing.test.sh` | 404, 405+Allow, OPTIONS without CORS, auto-HEAD |
| `cors.test.sh` | OPTIONS preflight when CORS on |

Shared helpers: `test/http/lib.sh` (`boot_quackapi`, `curl_json`, asserts).

## FastAPI conformance (live HTTP corpus)

```bash
bash test/conformance/run.sh
python3 test/conformance/render_scorecard.py
```

See `test/conformance/README.md`. Scorecard narrative: `docs/FASTAPI_PARITY.md`.

## SQL logic tests

```bash
# via DuckDB unittest (when configured with this tree as UNITTEST root)
./build/release/test/unittest "test/sql/quackapi*"
```

Or open `test/sql/*.test` under the DuckDB test runner used by `make test`.
