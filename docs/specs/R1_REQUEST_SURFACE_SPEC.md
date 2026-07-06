# R1 Request Surface — Pure-SQL Oracle Contract (for C brain mirror)

**Status:** Implemented (oracle side). This document is the authoritative spec for the C mirror in ext-cpp (brain.cpp and related). The SQL in framework.sql + middleware.sql + app.sql is the SSOT oracle. All behavior below must be reproduced byte-for-byte (modulo documented non-determinism like openapi key order).

**Date:** 2026-07-02  
**Scope (NATURAL gaps closed):** header params, cookie params, form-urlencoded bodies, Set-Cookie/redirect response helpers via route_headers, CORS middleware (pre/post via existing chain).  
**Non-scope:** ext-cpp edits (separate task; this spec + Tier-1 cases are the acceptance matrix). No changes to existing 16-case matrix outputs.

## 1. Inputs to handle_request (unchanged)
- `method VARCHAR`
- `path VARCHAR` (may contain ?query)
- `headers VARCHAR` (JSON object string; keys lowercased by C layer per brain.cpp:269-360; incoming cookies placed under `_cookies` sub-object per brain.cpp:300)
- `body VARCHAR` (raw; for JSON or form)

Example headers with cookie:
`{"authorization":"Bearer t","x-api-key":"k1","_cookies":{"session":"s1","other":"o"},"content-type":"application/x-www-form-urlencoded"}`

## 2. param_schema extensions
- `location` now also accepts `'header'` and `'cookie'` (in addition to `'path'|'query'|'body'`).
- Semantics for new locations:
  - `'header'`: `val_str := json_extract_string(headers, '$.' || name)` (name as declared, e.g. 'x_api_key' or 'x-api-key'; C lowercases header names before calling).
  - `'cookie'`: `val_str := json_extract_string( json_extract(headers, '$._cookies'), '$.' || name )` (NULL if no _cookies or missing key).
- All other columns (`type`, `required`, `constraint_json`) and processing identical to query params.
- OpenAPI emission (in framework.sql rendered_static for /openapi.json) now includes header/cookie params:
  `WHERE ps.route_id = ... AND ps.location IN ('path','query','header','cookie')`
  OpenAPI "in" uses the location value directly ("header", "cookie").

## 3. Form bodies (location='body')
- Detection: `lower(coalesce(json_extract_string(headers,'$.content-type'), json_extract_string(headers,'$.Content-Type'),'')) LIKE 'application/x-www-form-urlencoded%'`
- When true and body present: split body by `&`, split each pair by first `=`, apply `url_decode` (DuckDB built-in) to key and value (handles %XX and + → space).
- Parsed into `fmap` (map) via `map_from_entries(list_transform(...))`.
- In param_values for location='body':
  - if form ct: `fm.fmap[ps.name]`
  - else: original `json_extract_string(body, '$.' || ps.name)`
- Pure SQL, no regex (only string_split + list_filter/transform + built-in url_decode).
- Form values feed the exact same `val_str` → try_cast + constraint + required + 422 pipeline as JSON body params.
- Example: `name=zed&age=31` with ct=form + param age int → literal `31` (unquoted) in handler_sql.

url-decode table (for test writers):
- `+` → ` ` (space)
- `%20` → ` `
- `%2F` → `/`
- `%3A` → `:`
- etc. (standard percent)

## 4. Validation + 422 shapes (additive, identical rules)
- Same err_code rules apply to header/cookie/form vals (missing, int_parsing, float_parsing, bool_parsing, le/ge constraints).
- 422 body:
  `{"detail": [ {"type": "<err_code>", "loc": ["<location>", "<name>"], "msg": "<FastAPI-style message>"} , ... ] }`
- Exact loc shapes required (match FastAPI Header/Cookie):
  - missing header 'x_api_key' → `"loc": ["header", "x_api_key"]`
  - missing cookie 'session' → `"loc": ["cookie", "session"]`
- Msgs identical (see framework.sql err_agg for mapping).

## 5. route_headers + resp_headers return (additive)
- New table (created in framework.sql):
  ```sql
  CREATE OR REPLACE TABLE route_headers (
    route_id VARCHAR,
    name VARCHAR,
    value VARCHAR
  );
  ```
- Per-route static response headers. Set-Cookie is just a row (`name='Set-Cookie', value='...'`). Location for redirects too.
- In handle_request return (now 5 columns, additive):
  ```sql
  (status_code, content_type, body, handler_sql, resp_headers)
  ```
  `resp_headers` = `COALESCE( (SELECT json_group_object(name, value) FROM route_headers WHERE route_id = <matched>), '{}' )`
- For 404: `'{}'`
- For matched route (including validation 422 on that route, or redirect): the route's headers object.
- Existing 4-col selects and json_object extracts in tests/parity remain valid and produce identical values for old cases.

## 6. Redirects
- `register_redirect(route_id, method, pattern, target, status:=307)` macro:
  ```sql
  SELECT * FROM register_route(..., '', 'redirect', 'Redirect to '||target, status)
  ```
- Caller must:
  `INSERT INTO routes SELECT * FROM register_redirect('old','GET','/old','/new',307);`
  `INSERT INTO route_headers VALUES ('old','Location','/new');`
- In handle_request:
  - status = route.status (e.g. 307)
  - body = NULL
  - handler_sql = NULL
  - resp_headers contains the Location (and any other route_headers)
- Kind='redirect' treated like non-dynamic (no hsql).

## 7. CORS (via middleware chain, no change to handle_request core)
- Config row(s) in `middleware` table:
  ```json
  {
    "id":"cors", "phase":"pre", "priority":5, "kind":"cors",
    "config_json": "{\"allowed_origins\":[\"https://example.com\"], \"allowed_methods\":[\"GET\",\"POST\",\"OPTIONS\"], \"allowed_headers\":[\"Content-Type\",\"X-API-Key\"], \"max_age\":600}"
  }
  ```
- Pre-phase (apply_pre):
  - If `method='OPTIONS' AND origin present AND acr-method present AND cors config row exists`:
    - Short-circuit: pass=false, status=204, body='', content_type=application/json (unused), resp_headers populated with:
      - `Access-Control-Allow-Origin`: echoed origin if in allowed list or '*' present (list_contains on ::VARCHAR[])
      - `Access-Control-Allow-Methods`: comma-joined or default
      - `Access-Control-Allow-Headers`
      - `Access-Control-Max-Age`
  - Priority low (5) ensures before auth_gate.
- Post-phase (apply_post):
  - If any `kind='cors'` row with allowed_origins: merge `Access-Control-Allow-Origin` = '*' (if present) else first entry in list.
  - Uses json_merge_patch into existing resp_headers.
- apply_pre/apply_post return shapes extended additively with `resp_headers VARCHAR` (JSON object).
- Vary/credentials etc out of scope for R1.

## 8. Demo routes (in app.sql + duplicated in tier1 tests for oracle runs)
- `GET /secure` : header param `x_api_key` (required string)
- `GET /profile` : cookie param `session` (required string)
- `POST /form-submit` : body params (name str, age int) resolved via form or json depending on ct
- `GET /old-home` : redirect 307 + Location /new-home (via register_redirect + route_headers)
- `POST /login` : static 200 + Set-Cookie header via route_headers
- CORS enabled globally via middleware seed (origin https://example.com).

## 9. Tier-1 acceptance matrix (exact cases for C parity)
These are the new cases (beyond the original 16) that the C mirror must pass with identical status/body/headers (where headers means the 5th column resp_headers or middleware pre/post outputs).

Run via pure SQL (no server):
- framework + (middleware for cors) + test file(s)

**New happy/edge cases (implemented and asserted in tier1 + middleware.test):**

1. Header happy: GET /secure + `{"x_api_key":"k-123"}` → 200, resp_headers `{}`
2. Header missing required: GET /secure + `{}` → 422, body detail[0].type='missing', loc[0]='header', loc[1]='x_api_key'
3. Cookie happy: GET /profile + `{"_cookies":{"session":"s1"}}` → 200
4. Cookie missing: GET /profile + `{"_cookies":{}}` → 422, loc[0]='cookie', loc[1]='session'
5. Form happy: POST /form-submit + ct=form + `name=zed&age=31` → 200, handler_sql contains 'zed' (quoted) and 31 (bare)
6. Form bad type: POST /form-submit + ct=form + `name=zed&age=abc` → 422, type='int_parsing', loc[1]='age'
7. Redirect: GET /old-home → 307, body=NULL, handler_sql=NULL, resp_headers contains `{"Location":"/new"}` (or /new-home)
8. Set-Cookie: POST /login → 200, resp_headers contains `{"Set-Cookie":"..."}`
9. CORS preflight: apply_pre(OPTIONS, ..., headers with origin=https://example.com + acr-method=...) → pass=false, status=204, resp_headers.ACAO=origin, ACAM contains POST, etc.
10. CORS post: apply_post(200,...) → resp_headers contains ACOA from config first origin.

All old matrix cases (the 16 in parity_b2.sh) produce identical 4-col outputs (5th col `{} ` for routes without headers).

## 10. Implementation notes (oracle style)
- All in pure SQL macros/CTEs over routes/param_schema/route_headers/middleware tables.
- No regex (string_split, list_*, starts_with, replace, json_* only).
- Additive: old SELECTs, json_object on 4 cols, existing 16-case outputs unchanged.
- Comment style preserved (teaching artifact).
- handle_request self-checks at bottom of framework.sql continue to execute.

## 11. How C mirror will be verified
- Rebuild extension.
- Run parity_b2.sh (or equivalent) — must 16/16 on original matrix.
- Run the Tier-1 cases above (extended test files) — exact status + body + (resp_headers or pre/post headers).
- No behavior change to non-R1 paths.

This spec + the committed SQL + raw test/ parity outputs at implementation time are the contract.
