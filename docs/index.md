# quackapi

**A FastAPI-class web framework that lives inside DuckDB.**

You write routes. The database is the type system, the request validator, and the response encoder. One process serves HTTP and runs SQL.

```
browser  ──►  DuckDB [ quackapi ]
```

If you already know “routes,” you know enough to start.

---

## Five-line hello world

Build (once):

```sh
git clone --recurse-submodules https://github.com/asubbarao/quackapi
cd quackapi
GEN=ninja make release
./build/release/duckdb -unsigned
```

In the DuckDB shell:

```sql
LOAD quackapi;

CREATE ROUTE hello GET '/hello' AS SELECT 'world' AS msg;
CREATE ROUTE item  GET '/items/:id' AS SELECT $id::INTEGER AS id;

SELECT * FROM quackapi_serve(8000);
```

In another terminal (outputs below are from a live run against `build/release/duckdb -unsigned`):

```sh
curl http://127.0.0.1:8000/hello
# [{"msg":"world"}]

curl http://127.0.0.1:8000/items/42
# [{"id":42}]

curl http://127.0.0.1:8000/items/abc
# {"detail":[{"loc":["path","id"],"msg":"Input should be a valid integer","type":"type_error"}]}
# HTTP 422
```

That is the whole product idea:

1. **`CREATE ROUTE`** registers an HTTP endpoint whose handler is SQL.
2. **`$id::INTEGER`** (and friends) type-check request values.
3. Failures return **FastAPI-shaped 422** JSON: `{ "detail": [ { "loc", "msg", "type" } ] }`.
4. Success returns a **JSON array of row objects** by default — your `SELECT` list *is* the response model.

> **Parser tip:** run `LOAD quackapi;` first in a sequential session (interactive shell or FIFO stdin). Putting `LOAD` and `CREATE ROUTE` in one `duckdb -c "…"` string can parse the route before the extension is loaded.

---

## You write routes. Everything else is included.

| You need | You write | Built-in |
|----------|-----------|----------|
| HTTP methods | `GET` / `POST` / `PUT` / `PATCH` / `DELETE` / `HEAD` | 405 + `Allow`, auto-HEAD for GET |
| Path / query params | `$name::TYPE` or `PARAM name TYPE …` | 422 validation |
| JSON / form / multipart bodies | same `$name` namespace | body binder |
| Auth | `CREATE AUTH` + `REQUIRE` | API key + JWT HS256 |
| Instant list/get CRUD | `CREATE API FOR TABLE` | expands to two GET routes |
| Versioned prefix + shared auth | `CREATE GROUP` | APIRouter-style |
| Background jobs | `CREATE QUEUE` + enqueue/dequeue/ack | durable `quackapi_jobs` table |
| Live push | `CREATE STREAM … GET` | Server-Sent Events |
| Row security | `CREATE ROW ACCESS POLICY` | claims-keyed filters |
| Column redaction | `CREATE MASKING POLICY` | claims-keyed masks |
| Static files | `static_dir := '…'` on serve | unrouted GETs |
| OpenAPI | open the browser | `/openapi.json`, `/docs`, `/redoc` |

Start the server with:

```sql
SELECT * FROM quackapi_serve(8000);
-- → http://127.0.0.1:8000
```

Stop with:

```sql
SELECT * FROM quackapi_stop();
-- or SELECT * FROM quackapi_stop(8000);
```

---

## Mental model (one page)

### Request → bind → SQL → response

1. HTTP request hits a registered pattern (`/items/:id`).
2. Path, query, header, cookie, and body values land in the same **`$name`** parameter namespace.
3. Handler SQL runs with those parameters prepared.
4. Result rows become JSON (or `html` / `text` / redirect / Set-Cookie — see [headers, cookies, redirects](guide/headers-cookies-redirects.md)).

### Response envelope

| Handler shape | Response |
|---------------|----------|
| Multiple columns (default) | `application/json` **array of objects** |
| Single column named `html` | `text/html; charset=utf-8` raw body |
| Single column named `text` | `text/plain; charset=utf-8` raw body |
| Column `location` (+ `STATUS 3xx`) | redirect (`Location` header) |
| Column `set_cookie` / `set-cookie` | `Set-Cookie` header (stripped from JSON) |

Example of typed JSON (bool / number / null preserved):

```sql
CREATE ROUTE page_json GET '/json' AS
SELECT 'world' AS msg, 42 AS n, true AS ok, NULL::INTEGER AS missing;
```

```sh
curl http://127.0.0.1:8000/json
# [{"msg":"world","n":42,"ok":true,"missing":null}]
```

### Live registry

Routes created **after** serve is already running are live immediately. Inspect them:

```sql
SELECT name, method, pattern, status, require_auth
FROM quackapi_routes();
```

Handler SQL is validated at `CREATE` time. Broken SQL fails at create — not on first request.

---

## Guided tour (read top to bottom)

### Task guides

1. [Routes, typed params, and 422](guide/routes-and-params.md)  
2. [Request bodies (JSON, form, multipart, BODY SCHEMA)](guide/request-bodies.md)  
3. [Headers, cookies, redirects, status codes, content types](guide/headers-cookies-redirects.md)  
4. [Auth (API key + JWT) and REQUIRE](guide/auth.md)  
5. [CREATE API FOR TABLE (instant CRUD reads)](guide/table-api.md)  
6. [CREATE GROUP (prefix + shared auth = versioning)](guide/groups.md)  
7. [CREATE QUEUE (background jobs + worker)](guide/queue.md)  
8. [CREATE STREAM (SSE)](guide/stream.md)  
9. [Row access & masking policies](guide/policies.md)  
10. [Static files (`static_dir`)](guide/static-files.md)  
11. [OpenAPI, Swagger UI, ReDoc](guide/openapi.md)

### Reference

- [DDL grammar (every CREATE noun)](reference/ddl.md)  
- [SQL functions (`quackapi_*`)](reference/functions.md)

### FastAPI readers

- [FastAPI parity map (89/89)](fastapi-parity.md)  
- [Coming from FastAPI (`quack_from_X`)](from-fastapi.md)

### Feature ledger

Authoritative built-vs-not list: [FEATURE_STATUS.md](FEATURE_STATUS.md).

---

## Install & load

### From source (today)

```sh
GEN=ninja make release
./build/release/duckdb -unsigned
```

```sql
LOAD quackapi;
-- or absolute path:
-- LOAD '/path/to/build/release/extension/quackapi/quackapi.duckdb_extension';
```

### Community (after acceptance)

```sql
INSTALL quackapi FROM community;
LOAD quackapi;
```

**Target DuckDB:** v1.5.4. Platforms: Linux/macOS amd64+arm64. wasm and Windows are excluded until CI is green.

---

## Honest limits (so you plan correctly)

- **Single-writer DuckDB** — fine for many concurrent readers and light writes; not a pooled OLTP app tier.
- **JWT** — HS256 only today (no RS256 / OIDC browser flow yet).
- **`CREATE API FOR TABLE`** — read routes only (list + get by key).
- **WebSocket** — not supported on the HTTP transport; use [SSE streams](guide/stream.md).
- **Response gzip / access logging batteries** — [coming / in progress](guide/coming-soon.md).
- Registry (routes, auth, groups, queues, streams) lives on the **database instance**. Re-run DDL after reopen. Queue **jobs** (`quackapi_jobs`) are normal tables and survive restart.

---

## Next step

Open [Routes, typed params, and 422](guide/routes-and-params.md) and run every `curl` against your own `quackapi_serve` process.
