# quackapi test suite

Two tiers. Both are pure-DuckDB — no jq, no Python, no regex.

---

## Tier 1 — `handle_request()` direct assertions

Calls the `handle_request(method, path, headers, body)` TABLE macro in-memory
and asserts on `(status_code, content_type, body, handler_sql)` for every demo
route plus 404 and 422 cases. Results land in a `_test_results` table printed
at the end. A final summary row shows `total / passed / failed`.

**Run:**

```bash
printf '.read framework.sql\n.read test/tier1_handle_request.test.sql\n' \
  | duckdb
```

No server needed. Runs entirely in-memory.

**What it covers (62 checks):**

| Route | Checks |
|-------|--------|
| `GET /users/123` | status=200, body=NULL, handler_sql rendered, contains 123 |
| `GET /users/abc` | status=422, err_type=int_parsing, err_loc=id |
| `GET /nope` | status=404, detail=Not Found |
| `GET /search?q=hi&limit=999` | status=422, err_type=less_than_equal, field=limit |
| `GET /search?q=hi&limit=5` | status=200, handler_sql not null |
| `GET /search?q=hi` | status=200 (limit optional) |
| `GET /search?limit=5` (missing q) | status=422, err_type=missing, field=q |
| `GET /search?q=hi&limit=abc` | status=422, err_type=int_parsing, field=limit |
| `GET /users` | status=200, handler_sql=SELECT list_users |
| `GET /users/7/posts/99` | status=200, body=NULL, both params interpolated |
| `POST /users {"name","age"}` | status=200, handler_sql=INSERT, name interpolated |
| `POST /users {"name"}` (missing age) | status=422, err_type=missing on age |
| `POST /users {}` (both missing) | status=422, 2 errors |
| `GET /openapi.json` | status=200, $.openapi=3.0.0, $.info.title=quackapi, $.paths is object |
| `GET /docs` | status=200, text/html, DOCTYPE, swagger-ui div |
| `GET /whoami` | status=200, handler_sql not null |

---

## Tier 2 — `tier2_http.sh` live HTTP assertions

Hits a running server with curl. All JSON parsing done via DuckDB (`dq()` helper
calls `duckdb -noheader -list`). No regex, no jq.

**Start the server first:**

```bash
duckdb < launch_server.sql &   # run from the repo root
```

**Run (read-only, safe against the live :18099 server):**

```bash
bash test/tier2_http.sh                          # default http://127.0.0.1:18099
bash test/tier2_http.sh http://127.0.0.1:18099   # explicit BASE_URL
```

**Run with POST (mutates the users table — use a throwaway DB):**

```bash
bash test/tier2_http.sh http://127.0.0.1:18099 --post
```

If the server is unreachable the script exits 2 with instructions.

**What it covers (17 read-only + 3 with --post = 20 checks):**

| Route | Check |
|-------|-------|
| `GET /users/1` | status=200, body not empty, $.id=1 |
| `GET /users/abc` | status=422, detail has int_parsing |
| `GET /users` | status=200, body starts with `[` |
| `GET /openapi.json` | status=200, $.openapi=3.0.0, $.info.title=quackapi, $.paths is object |
| `GET /docs` | status=200, starts with `<!DOCTYPE html>`, swagger-ui div present |
| `GET /nope` | status=404 |
| `GET /search?q=hi&limit=999` | status=422, detail has less_than_equal |
| `POST /users` valid (--post) | status=200, $.name=tiertest |
| `POST /users` missing age (--post) | status=422 |

---

## Constraints

Both files obey the quackapi query hook rules:

- No `LIKE` / `ILIKE` / regex / `SIMILAR TO`
- No `COUNT(*)` — use `array_length(array_agg(x))`
- No literal `VALUES` rows — all data comes from `handle_request()` or live HTTP
- Lambda syntax: `lambda x: expr`
- DuckDB v1.5.3 at `duckdb`
