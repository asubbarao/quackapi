# quackapi

**A FastAPI-class web framework that lives inside DuckDB.**

Routes are DDL. Handlers are SQL. Request validation is the database type system.
One process is the database, the HTTP server, and — when you load companion
extensions — the PDF engine, HTML renderer, crawler, and fake-data generator.

```
browser ──► DuckDB [ quackapi · pdf · tera · fakeit · webbed · … ]
```

There is no app server, no ORM, and no schema re-declared in a second language.
The query already types the response.

> **Thesis:** the backend *is* the database.  
> Prior art: PostgREST, Datasette, Hasura, Oracle APEX. The novel part is one
> **embedded OLAP** process as DB + HTTP framework + (optional) PDF/renderer,
> driven by `CREATE ROUTE` DDL.

---

## Five-line quickstart

Build locally (community install is not live until the extension is accepted):

```sh
git clone --recurse-submodules https://github.com/asubbarao/quackapi
cd quackapi
GEN=ninja make release
./build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/quackapi/quackapi.duckdb_extension';

CREATE ROUTE hello GET '/hello' AS SELECT 'world' AS msg;
CREATE ROUTE item  GET '/items/:id' AS SELECT $id::INTEGER AS id;

SELECT * FROM quackapi_serve(8000);
```

```sh
curl http://127.0.0.1:8000/hello
# [{"msg":"world"}]

curl http://127.0.0.1:8000/items/42
# [{"id":42}]

curl http://127.0.0.1:8000/items/abc
# {"detail":[{"loc":["path","id"],"msg":"…","type":"type_error"}]}
# HTTP 422 — FastAPI-shaped validation error
```

> **Parser note:** `CREATE ROUTE` is registered when you `LOAD quackapi`. Feed
> `LOAD` first in a sequential session (interactive shell or stdin), then the
> DDL. Putting `LOAD` and `CREATE ROUTE` in one `duckdb -c "…"` string can parse
> the route before the extension is loaded.

---

## CREATE ROUTE — the model

```sql
CREATE [OR REPLACE] ROUTE <name> <METHOD> '<pattern>'
  [STATUS <n>]
  [REQUIRE <auth>]
  [GROUP <group> | IN GROUP <group>]
  [BODY SCHEMA '<json-schema>']
  [PARAM <name> [<type>] [HEADER|COOKIE|QUERY [wire-name]]
         [DEFAULT <lit>] [GE|GT|LE|LT <n>] [MIN_LENGTH|MAX_LENGTH <n>] …]
  AS <select>;

DROP ROUTE <name>;
```

| Piece | Behavior |
|-------|----------|
| **Method** | `GET`, `POST`, `PUT`, `DELETE`, `PATCH`, `HEAD` |
| **Path** | Must start with `/` (unless joined via `GROUP`). `:param` and `{param}` bind to `$param` |
| **Handler** | Any SQL statement that returns a result (typically `SELECT` / `INSERT … RETURNING` / `COPY`) |
| **Params** | Cast to the types the prepared handler expects; failures → **422** with FastAPI-shaped `detail` |
| **Success status** | Default `200`; override with `STATUS 201` (etc.) |
| **Auth** | `REQUIRE <scheme>` runs a `CREATE AUTH` scheme before the handler |

Live updates: a route created after `quackapi_serve()` is served immediately.
Handler SQL is validated at `CREATE` time (broken SQL fails at create, not on
first request). `CREATE OR REPLACE` does not leave a half-applied route on
failure.

### Response modes (column names)

| Column name | Response |
|-------------|----------|
| *(default)* | JSON **array of row objects** — types follow DuckDB column types |
| single column `html` | `text/html; charset=utf-8` (raw string body) |
| single column `text` | `text/plain; charset=utf-8` |
| `location` | `Location` header (use with `STATUS 3xx` for redirects) |
| `set_cookie` / `set-cookie` | `Set-Cookie` header (stripped from JSON body) |

### JSON route

```sql
CREATE ROUTE get_doc GET '/api/documents/:id' AS
SELECT * FROM documents WHERE id = $id::INTEGER;
```

### HTML route (`html` column)

```sql
-- With the community tera extension:
-- INSTALL tera FROM community; LOAD tera;
CREATE ROUTE dashboard GET '/cases/:id' AS
SELECT tera_render(template, ctx) AS html
FROM app_templates, …
WHERE name = 'case.html';
```

Without tera, any string column named `html` works:

```sql
CREATE ROUTE home GET '/' AS
SELECT '<h1>quackapi</h1>' AS html;
```

### Static files

```sql
SELECT * FROM quackapi_serve(
  8000,
  host := '127.0.0.1',
  static_dir := './static'   -- unrouted GETs served from this directory
);
```

### Typed params + 422 validation

```sql
CREATE ROUTE search GET '/search'
  PARAM limit INTEGER DEFAULT 10 LE 100
  AS
SELECT id, name FROM users
WHERE name ILIKE $q::VARCHAR || '%'
ORDER BY id
LIMIT $limit::INTEGER;
```

```sh
curl 'http://127.0.0.1:8000/search?q=a&limit=101'
# 422  loc=["query","limit"]  type=less_than_equal

curl 'http://127.0.0.1:8000/search?q=a&limit=abc'
# 422  loc=["query","limit"]  type=type_error
```

Path / query / JSON body / form / multipart all bind into the same `$name`
namespace. JSON object fields become params; `$body` binds the raw payload when
the handler declares it. Missing required params → 422; unknown path → 404;
wrong method → 405 (+ `Allow`).

### POST mutation

```sql
CREATE ROUTE decide POST '/api/suggestions/:id/decision' STATUS 201
  PARAM status VARCHAR
  PARAM actor  VARCHAR DEFAULT 'reviewer'
  AS
INSERT INTO decisions BY NAME
SELECT $id::INTEGER AS suggestion_id,
       $status AS status,
       $actor  AS actor
RETURNING *;
```

```sh
curl -X POST http://127.0.0.1:8000/api/suggestions/1/decision \
  -H 'Content-Type: application/json' \
  -d '{"status":"accepted"}'
```

---

## Showcase: Closure (one process)

**Closure** (companion app in the same workspace) — an AI-assisted PDF redaction-review
app — runs entirely as **one DuckDB process**:

| Concern | Implementation |
|---------|----------------|
| HTTP routes | quackapi `CREATE ROUTE` + `quackapi_serve` |
| PDF words / redact | community `pdf` (`read_pdf_words`, `pdf_redact`, …) |
| HTML UI | community `tera` (`tera_render(…) AS html`) |
| Data + audit | ordinary DuckDB tables / views |
| Static page images | `static_dir` |

HTML dashboards, JSON APIs, and POST decision endpoints are all SQL. Calling
the “PDF service” is a function call in the same address space — not an RPC.

---

## Function & DDL reference

### Serving

| Surface | Signature / form | Returns |
|---------|------------------|---------|
| `quackapi_serve` | `([port], host := …, memory_limit := …, http_client := 'auto'\|'curl'\|'httplib', …)` | `listen_url` |
| `quackapi_stop` | `([port])` — omit port to stop all | `status` |
| `quackapi_routes` | `()` | `name, method, pattern, status, handler, require_auth, group_name, tags` |
| `quackapi_servers` | `()` | `host, port, listen_url, http_client` |
| Setting | `SET quackapi_cors_origins = '*' \| 'https://a,https://b'` | empty = CORS off |
| Setting | `SET quackapi_memory_limit = '4GB' \| '512MB' \| …` | empty = non-clobber default logic |
| Setting | `SET quackapi_http_client = 'auto' \| 'curl' \| 'httplib'` | prefer curl_httpfs outbound client |

Built-in OpenAPI (not listed in `quackapi_routes()`):

- `GET /openapi.json` — OpenAPI 3.1 from the live registry  
- `GET /docs` — Swagger UI  
- `GET /redoc` — ReDoc  

**Serve memory limit** (never silently clobbers an operator setting):

1. `memory_limit := '…'` named parameter on `quackapi_serve` wins  
2. else `SET quackapi_memory_limit = '…'`  
3. else if DuckDB already has a **non-default** `memory_limit` → leave it alone  
4. else apply the safe default of **256MB**

```sql
-- App that needs headroom (PDF/HTML workloads, large joins, etc.)
SELECT * FROM quackapi_serve(8000, memory_limit := '4GB');

-- Or configure once for the session, then serve:
SET quackapi_memory_limit = '4GB';
SELECT * FROM quackapi_serve(8000);

-- Or set DuckDB's limit yourself — serve will not overwrite it:
SET memory_limit = '4GB';
SELECT * FROM quackapi_serve(8000);
```

Request body limit: **8 MiB** (`413` when exceeded).

### Auth

```sql
CREATE [OR REPLACE] AUTH <name> AS API_KEY [( HEADER 'X-Custom-Key' )];
CREATE [OR REPLACE] AUTH <name> AS JWT ( SECRET '…' [, ALGORITHM HS256] );
DROP AUTH <name>;

SELECT * FROM quackapi_add_api_key('site', 'raw-key', 'alice');  -- stores SHA-256 only
SELECT * FROM quackapi_auths();   -- name, kind, header — never secrets/hashes
```

Verified JWT/API-key claims bind as `$claims_<name>` (missing claim → SQL `NULL`,
not 422). Bridge scalars for the core quack RPC: `quackapi_authentication`,
`quackapi_authorization`, `quackapi_verify_auth`.

### Route groups (APIRouter-style)

```sql
CREATE GROUP v1 WITH (prefix='/api/v1', auth=api, tags='items,v1');
CREATE ROUTE items_list GET '/items' GROUP v1 AS SELECT …;
-- served at GET /api/v1/items, inherits auth + tags
SELECT * FROM quackapi_groups();
```

### Table API (read-only scaffold)

```sql
CREATE API FOR TABLE documents [AT '/docs'] [KEY 'id'];
-- registers GET /docs and GET /docs/:id only (no write routes)
```

### Durable job queue (broker-less)

Jobs live in the ordinary table `quackapi_jobs` in your `.db` file — no Redis.

```sql
CREATE QUEUE emails WITH (max_attempts=5, visibility_timeout='30s');

CREATE ROUTE enqueue POST '/jobs' STATUS 201 PARAM payload VARCHAR AS
SELECT quackapi_enqueue('emails', $payload) AS job_id;

SELECT id, payload, quackapi_ack('emails', id) AS acked
FROM quackapi_dequeue('emails', 10);
```

| Function | Role |
|----------|------|
| `quackapi_enqueue(queue, payload [, max_attempts])` | → `job_id` |
| `quackapi_dequeue(queue [, n])` | claim with visibility lease |
| `quackapi_ack` / `quackapi_nack` | complete or retry / dead-letter |
| `quackapi_queues()` | depth, in_flight, dead, options |

Worker = compose community `cronjob` (or a drain route). See [`docs/QUEUE.md`](docs/QUEUE.md).

### SSE streams

```sql
CREATE STREAM ticks GET '/events' AS SELECT …;   -- text/event-stream
-- WebSocket methods are rejected (bundled httplib has no Upgrade API)
SELECT * FROM quackapi_streams();
```

### Row-access & masking policies

```sql
CREATE ROW ACCESS POLICY tenant_isolation AS (tenant_id) RETURNS BOOLEAN
  USING (tenant_id = $claims_tenant);
CREATE MASKING POLICY mask_email ON VARCHAR USING (…);
ALTER TABLE … ADD ROW ACCESS POLICY …;
SELECT * FROM quackapi_policies();
```

### Diagnostics

| Function | Role |
|----------|------|
| `quackapi_http_util_name()` | name of the active outbound HTTPUtil (`Built-In`, `MultiCurl` after `LOAD curl_httpfs`, …) |

Outbound HTTPS uses DuckDB’s shared `HTTPUtil` (no libcurl linked into quackapi).
`quackapi_serve` **batteries prefer `curl_httpfs`** (pool + HTTP/2 + async) and fall
back to httplib when the community extension is unavailable — see
[`docs/curl_httpfs.md`](docs/curl_httpfs.md).

---

## Install & load

### From source (today)

```sh
GEN=ninja make release
./build/release/duckdb -unsigned   # extension available under build/release/extension/quackapi/
```

```sql
LOAD '/absolute/path/to/quackapi.duckdb_extension';
```

### Community (after acceptance)

```sql
INSTALL quackapi FROM community;
LOAD quackapi;
```

**Target DuckDB:** **v1.5.4** (pinned in CI: `.github/workflows`, submodule `duckdb` @ `v1.5.4`).

**Signed-build platforms** (community CI / extension-ci-tools):

| Platform | Status |
|----------|--------|
| `linux_amd64`, `linux_arm64` | built (target) |
| `osx_amd64`, `osx_arm64` | built (target) |
| `wasm_*` | **excluded** (no server sockets) |
| `windows_amd64`, `windows_amd64_mingw`, `windows_amd64_rtools`, `windows_arm64` | **excluded** (unproven in repo CI; re-opt-in after green MSVC build) |

Dependencies: C++17, DuckDB’s **bundled httplib** + **mbedtls** only — no vcpkg,
no libcurl, no extra toolchains (`requires_toolchains` not needed).

---

## Behavior notes

- **Response envelope:** always a JSON array of row objects for multi-column /
  non-`html`/`text` handlers (SQL result-set semantics — not a bare object).
- **CORS:** off by default; enable with `cors_origins` / `SET quackapi_cors_origins`.
- **Registry lifecycle:** routes, auth schemes, groups, queues, streams live on
  the **database instance** (not the catalog). Re-run DDL after reopen.
- **Queue jobs** (`quackapi_jobs`) *are* catalog tables and survive restart.
- **Concurrency:** DuckDB single-writer per file — fine for a few concurrent
  reviewers; not a high-write multi-tenant OLTP app server.
- **FastAPI parity:** real-HTTP conformance suite in `test/conformance/` and
  `test/http/` — see [`docs/FASTAPI_PARITY.md`](docs/FASTAPI_PARITY.md).

---

## Security

- **Default bind is `127.0.0.1`** — the server is loopback-only unless you opt
  in with `host := '0.0.0.0'` (or `SET quackapi_host`).
- **Binding a non-loopback host exposes your SQL-backed routes to the
  network.** Anything a route's SQL can read, a caller can read. Before doing
  so, put `CREATE AUTH` (API key or JWT) plus `CREATE GROUP` / row policies in
  front of every route, or terminate at a reverse proxy that handles TLS and
  auth.
- **No TLS termination in-process** — use a reverse proxy (nginx/caddy) for
  HTTPS.
- Treat the extension as **experimental**: don't point it at production data
  on an exposed interface.

---

## Honest limits

- **Write concurrency / OLTP:** single-writer semantics; not a replacement for
  a connection-pooled app tier under heavy concurrent writes.
- **Unsigned until community acceptance:** local / CI builds only until the
  community-extensions PR lands and binaries are signed.
- **Serve memory default:** `quackapi_serve` applies a 256MB `memory_limit`
  only when nothing was configured (no `memory_limit` / `quackapi_memory_limit`
  and DuckDB is still at its system default). Prefer
  `memory_limit := '4GB'` (or `SET quackapi_memory_limit`) for large PDF/HTML
  workloads — it will never clobber an explicit operator `SET memory_limit`.
- **No WebSocket routes** on the HTTP transport (SSE via `CREATE STREAM` only).
- **`CREATE API FOR TABLE`** scaffolds **GET list + GET by key** only.
- **JWT:** HS256 only (`ALGORITHM HS256`); no RS256 / OIDC discovery yet.
- **Not a general multi-tenant SaaS framework:** policies are claims-keyed
  helpers, not a full RBAC product.

---

## Build & test

```sh
GEN=ninja make release
make test                          # SQL unit tests
bash test/http/run_all.sh          # live curl suite (needs release build)
bash test/conformance/run.sh       # FastAPI parity harness
```

## Docs in this repo

| Doc | Content |
|-----|---------|
| [`docs/community-page.md`](docs/community-page.md) | Community-extensions page copy |
| [`packaging/description.yml`](packaging/description.yml) / [`description.yml`](description.yml) | Community submission manifest |
| [`docs/FASTAPI_PARITY.md`](docs/FASTAPI_PARITY.md) | Scorecard vs FastAPI |
| [`docs/QUEUE.md`](docs/QUEUE.md) | Job queue semantics |
| [`docs/curl_httpfs.md`](docs/curl_httpfs.md) | Outbound HTTP via curl_httpfs |

## License

MIT — Copyright (c) 2026 Alok Subbarao
