# quackapi — Community Extensions documentation

> Draft content for the hosted page at  
> `https://duckdb.org/community_extensions/extensions/quackapi`  
> (generated from `description.yml` fields + auto-detected functions after load).  
> This file is the fuller, source-verified reference used to keep those fields honest.

---

## Overview

**quackapi** is a FastAPI-class HTTP framework that runs **inside** DuckDB.

- **Routes are DDL** (`CREATE ROUTE`).
- **Handlers are SQL** (the query *is* the endpoint).
- **Validation is DuckDB’s type system** — cast failures become FastAPI-shaped **422** bodies.
- **One process** serves as the database and the HTTP server. Load companion
  community extensions (`pdf`, `tera`, `fakeit`, `cronjob`, `curl_httpfs`, …)
  and the same process can also render HTML, redact PDFs, crawl pages, and
  schedule SQL workers — no microservice mesh.

```
browser → DuckDB [ quackapi · pdf · tera · fakeit · webbed · cronjob · … ]
```

**Prior art:** PostgREST / Supabase, Datasette, Hasura, Oracle APEX.  
**Difference:** an embedded OLAP engine as the entire backend surface, with
`CREATE ROUTE` instead of a separate app runtime.

**Showcase:** *Closure* — a PDF redaction-review app — boots with
`LOAD quackapi` + `CREATE ROUTE` + `quackapi_serve`, using `pdf` for word boxes
and `pdf_redact`, `tera` for `… AS html` pages, and ordinary tables for audit.
Zero external app servers.

---

## Install

After community acceptance and signed publish:

```sql
INSTALL quackapi FROM community;
LOAD quackapi;
```

Until then, build from source against **DuckDB v1.5.4**:

```sh
git clone --recurse-submodules https://github.com/asubbarao/quackapi
cd quackapi && GEN=ninja make release
./build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/quackapi/quackapi.duckdb_extension';
```

> `CREATE ROUTE` is a **parser extension** registered at `LOAD` time. Run `LOAD`
> before route DDL in a sequential session (interactive shell or stdin). A single
> `duckdb -c "LOAD …; CREATE ROUTE …"` can parse the route before load completes.

---

## The CREATE ROUTE model

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

| Element | Detail |
|---------|--------|
| Method | `GET` `POST` `PUT` `DELETE` `PATCH` `HEAD` |
| Pattern | Absolute path starting with `/` (or relative when using `GROUP`) |
| Path params | `:id` or `{id}` → `$id` |
| Query / body / form / multipart | Same `$name` binding; `$body` for raw payload when declared |
| STATUS | Default 200 |
| REQUIRE | Named scheme from `CREATE AUTH` |
| PARAM | Optional types, defaults, numeric/string constraints, header/cookie sources |
| BODY SCHEMA | Optional JSON Schema string for request body validation |
| Handler | Prepared at CREATE; invalid SQL fails CREATE (not first request) |

Routes update **live** while the server is running.

### Response modes

| Handler shape | HTTP body |
|---------------|-----------|
| Multiple columns / ordinary names | `application/json` — **array of row objects** |
| Single column `html` | `text/html; charset=utf-8` |
| Single column `text` | `text/plain; charset=utf-8` |
| Column `location` | sets `Location` (pair with `STATUS 3xx`) |
| Column `set_cookie` / `set-cookie` | sets `Set-Cookie` (stripped from JSON) |

JSON types follow DuckDB column types (numbers, booleans, nulls, nested lists/structs).

### Error shapes (verified)

| Situation | Status |
|-----------|--------|
| Type / constraint / missing required param | **422** `{"detail":[{loc,msg,type},…]}` |
| Unknown path | **404** |
| Wrong method | **405** + `Allow` |
| Auth failure | **401** |
| Body too large (> 8 MiB) | **413** |
| Handler internal error | **500** `{"detail":"Internal Server Error"}` (SQL text not leaked) |

---

## Examples

### JSON route

```sql
CREATE ROUTE get_doc GET '/api/documents/:id' AS
SELECT * FROM documents WHERE id = $id::INTEGER;
```

### HTML route

```sql
CREATE ROUTE home GET '/' AS
SELECT '<h1>quackapi</h1>' AS html;

-- With community tera:
-- SELECT tera_render(template, ctx) AS html FROM …;
```

### Static directory + CORS

```sql
SET quackapi_cors_origins = 'https://app.example';
SELECT * FROM quackapi_serve(
  8000,
  host := '0.0.0.0',
  static_dir := './static',
  cors_origins := '*'          -- named param overrides SET
);
```

### Typed params + 422

```sql
CREATE ROUTE search GET '/search'
  PARAM limit INTEGER DEFAULT 10 LE 100
  AS
SELECT id, name FROM users
WHERE name ILIKE $q::VARCHAR || '%'
LIMIT $limit::INTEGER;
```

`limit=101` → 422 `less_than_equal`; `limit=abc` → 422 `type_error` with
`loc=["query","limit"]`.

### POST mutation

```sql
CREATE ROUTE decide POST '/api/suggestions/:id/decision' STATUS 201
  PARAM status VARCHAR
  PARAM actor VARCHAR DEFAULT 'reviewer'
  AS
INSERT INTO decisions BY NAME
SELECT $id::INTEGER AS suggestion_id, $status AS status, $actor AS actor
RETURNING *;
```

```sh
curl -X POST http://127.0.0.1:8000/api/suggestions/1/decision \
  -H 'Content-Type: application/json' \
  -d '{"status":"accepted"}'
```

### Auth

```sql
CREATE AUTH site AS API_KEY;  -- default header X-API-Key
SELECT * FROM quackapi_add_api_key('site', 'super-secret', 'alice');

CREATE ROUTE me GET '/me' REQUIRE site AS
SELECT $claims_sub AS user;
```

```sql
CREATE AUTH jwt_auth AS JWT ( SECRET 'test-secret', ALGORITHM HS256 );
```

### Serve / inspect / stop

```sql
SELECT * FROM quackapi_serve(8000);
SELECT * FROM quackapi_routes();
SELECT * FROM quackapi_servers();
SELECT * FROM quackapi_stop(8000);
```

Built-in (not in `quackapi_routes()`): `/openapi.json`, `/docs`, `/redoc`.

---

## Function table

Surfaces registered by the extension (source: `src/quackapi_extension.cpp` and
related modules). Auto-detection on the community site will list overloads after
`LOAD`; this table is the human guide.

### HTTP lifecycle

| Name | Kind | Purpose |
|------|------|---------|
| `quackapi_serve([port], host, static_dir, cors_origins)` | table | Start listener (default `127.0.0.1:8000`) |
| `quackapi_stop([port])` | table | Stop one or all servers |
| `quackapi_routes()` | table | Registry: name, method, pattern, status, handler, require_auth, group_name, tags |
| `quackapi_servers()` | table | host, port, listen_url |
| `quackapi_cors_origins` | setting | CORS allow-list (`*` or CSV); empty = off |

### Auth

| Name | Kind | Purpose |
|------|------|---------|
| `CREATE` / `DROP AUTH` | DDL | API_KEY or JWT HS256 schemes |
| `quackapi_add_api_key(auth, key, subject)` | table | Store SHA-256 hash of key |
| `quackapi_auths()` | table | name, kind, header (no secrets) |
| `quackapi_verify_auth(scheme, auth_string)` | scalar | Policy engine probe |
| `quackapi_authentication` / `quackapi_authorization` | scalar | Bridges for core quack RPC settings |

### Groups

| Name | Kind | Purpose |
|------|------|---------|
| `CREATE` / `DROP GROUP` | DDL | Prefix + default auth + OpenAPI tags |
| `quackapi_groups()` | table | name, prefix, require_auth, tags, members |

### Table API

| Name | Kind | Purpose |
|------|------|---------|
| `CREATE API FOR TABLE …` | DDL | Registers GET list + GET by key only |

### Queue

| Name | Kind | Purpose |
|------|------|---------|
| `CREATE` / `DROP QUEUE` | DDL | Register queue options |
| `quackapi_enqueue` | scalar | Enqueue JSON/text payload → job_id |
| `quackapi_dequeue` | table | Claim up to n jobs |
| `quackapi_ack` / `quackapi_nack` | scalar | Complete / retry / dead-letter |
| `quackapi_queues()` | table | depth, in_flight, dead, options |
| `quackapi_jobs` | table (catalog) | Durable job rows |

### Streams & policies

| Name | Kind | Purpose |
|------|------|---------|
| `CREATE` / `DROP STREAM` | DDL | SSE (`GET` only); WS rejected |
| `quackapi_streams()` | table | Inspect streams |
| `CREATE ROW ACCESS POLICY` / `CREATE MASKING POLICY` | DDL | Claims-oriented policies |
| `ALTER TABLE …` policy bind | DDL | Attach/detach |
| `quackapi_policies()` | table | Inspect policies |

### Other

| Name | Kind | Purpose |
|------|------|---------|
| `quackapi_http_util_name()` | scalar | Active outbound HTTPUtil name |

Internal apply helpers (`quackapi_apply_route`, `quackapi_apply_auth`, …) exist
for the planner and are not part of the public app API.

---

## Platforms & signed-build feasibility

| Item | Value |
|------|--------|
| Target DuckDB | **v1.5.4** (CI + submodule pin) |
| Language / build | C++17 / **cmake** |
| Extra toolchains | **None** (no vcpkg, no Rust, no Python at build) |
| Linked deps | DuckDB **bundled httplib** + **mbedtls** only |
| Outbound HTTPS | Core `HTTPUtil`; optional `LOAD curl_httpfs` upgrades process-wide client |

| DuckDB arch | Community CI intent |
|-------------|---------------------|
| `linux_amd64`, `linux_arm64` | Build & sign |
| `osx_amd64`, `osx_arm64` | Build & sign |
| `wasm_mvp`, `wasm_eh`, `wasm_threads` | **Excluded** — no server sockets |
| `windows_amd64`, `windows_amd64_mingw`, `windows_amd64_rtools`, `windows_arm64` | **Excluded** — unproven in repo CI (httplib is portable; re-opt-in after green MSVC) |

Repo workflow: `duckdb/extension-ci-tools` `@v1.5-variegata`,
`duckdb_version: v1.5.4`, same `exclude_archs` as `description.yml`.

**Feasibility:** high for the non-excluded platforms — pure CMake extension
template shape, no exotic system libraries. Blocker before first signed publish:
clean release tag with **no merge-conflict markers** in `src/`, green
`make release` + `test/http/run_all.sh` on at least one host arch.

---

## Honest limits

1. **Single-writer OLTP** — DuckDB file locking; suitable for small concurrent
   reviewer sets, not high-write multi-tenant SaaS.
2. **Instance-scoped registry** — routes/auth/groups/streams/queue *options*
   die with the process; re-run DDL on boot. Job **rows** in `quackapi_jobs`
   persist.
3. **Serve memory guard** — `quackapi_serve` sets `memory_limit` to 256MB;
   raise after serve for large PDF/HTML work.
4. **Body size** — 8 MiB max payload.
5. **JWT** — HS256 only.
6. **Table API** — read-only GET scaffold.
7. **No WebSocket** on the HTTP sidecar (SSE via `CREATE STREAM` only).
8. **Unsigned until accepted** — community `INSTALL` path goes live after the
   community-extensions PR and signing pipeline succeed.

---

## Related links

- Source: [github.com/asubbarao/quackapi](https://github.com/asubbarao/quackapi)
- FastAPI parity scorecard: [`docs/FASTAPI_PARITY.md`](FASTAPI_PARITY.md)
- Queue semantics: [`docs/QUEUE.md`](QUEUE.md)
- Outbound HTTP: [`docs/curl_httpfs.md`](curl_httpfs.md)
- Submission YAML: [`description.yml`](../description.yml)
