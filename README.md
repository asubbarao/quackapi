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

This README documents the source in this checkout. Community installation uses
the commit pinned in the [DuckDB extension registry](https://github.com/duckdb/community-extensions/blob/main/extensions/quackapi/description.yml),
so new APIs become available there only after that descriptor is updated and
its signed builds are published. See [migration notes](docs/MIGRATION.md) when
upgrading an existing application.

> **Thesis:** the backend *is* the database.  
> Prior art: PostgREST, Datasette, Hasura, Oracle APEX. The novel part is one
> **embedded OLAP** process as DB + HTTP framework + (optional) PDF/renderer,
> driven by `CREATE ROUTE` DDL.
>
> **Inspiration:** quackapi was also inspired by the Query.Farm [`httpserver`](https://github.com/Query-farm/httpserver) DuckDB community extension.

---

## Five-line quickstart

Requires **DuckDB v2.0.0** (linux, macOS, windows_amd64). Builds target the stable
release and use DuckDB's PEG parser token interface for custom DDL. JSON responses
preserve native `VARIANT` objects, lists, scalars and nulls.

`BODY SCHEMA` uses `json_schema`, distributed from Query.Farm on DuckDB 2.0.
Register its native extension repository before creating schema-validated routes
(or install and load the dependency explicitly during startup):

```sql
SET allow_extension_repositories = 'allowed';
CREATE EXTENSION REPOSITORY IF NOT EXISTS query_farm
  WITH PREFIX 'https://duckdb-extensions.query.farm';
INSTALL json_schema FROM query_farm;
LOAD json_schema FROM query_farm;
```

Optional `cronjob` scheduling uses the same repository. The network scheduling
test runs with `QUACKAPI_NETWORK_TESTS=1`; the Postgres CI job enables it.

```sql
INSTALL quackapi FROM community;
LOAD quackapi;

CREATE ROUTE hello GET '/hello' AS SELECT 'world' AS msg;
CREATE ROUTE item  GET '/items/:id' AS SELECT $id::INTEGER AS id;

SELECT * FROM quackapi_serve(8000);
-- returns immediately; pass block := true to hold until stop / SIGINT / SIGTERM

-- Readiness (replaces sleep/lsof/curl loops):
-- SELECT ready, listen_url FROM quackapi_wait(8000, 5000);
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
  [SCHEDULE '<6-field cron expression, seconds first>']
  [GROUP <group> | IN GROUP <group>]
  [BODY SCHEMA '<json-schema>']
  [BODY TYPE '<duckdb-json-transform-structure>']
  [PARAM <name> [<type>] [HEADER|COOKIE|QUERY [wire-name]]
         [DEFAULT <lit>] [GE|GT|LE|LT <n>] [MIN_LENGTH|MAX_LENGTH <n>] …]
  AS <select> | AS $<param>;

DROP ROUTE <name>;
```

| Piece | Behavior |
|-------|----------|
| **Method** | `GET`, `POST`, `PUT`, `DELETE`, `PATCH`, `HEAD` |
| **Path** | Must start with `/` (unless joined via `GROUP`). `:param` and `{param}` bind to `$param` |
| **Handler** | Any SQL statement that returns a result (typically `SELECT` / `INSERT … RETURNING` / `COPY`) |
| **Exec handler** | Exactly one parameter, `AS $sql`: the request's bound text is executed as SQL — see [Exec routes](#exec-routes-as-sql) |
| **Params** | Cast to the types the prepared handler expects; failures → **422** with FastAPI-shaped `detail` |
| **Success status** | Default `200`; override with `STATUS 201` (etc.) |
| **Auth** | `REQUIRE <scheme>` runs a `CREATE AUTH` scheme before the handler |
| **Schedule** | Optional six-field cron expression (`seconds minutes hours day-of-month month day-of-week`); stored on the route and registered by the community `cronjob` extension |

Live updates: a route created after `quackapi_serve()` is served immediately.
Handler SQL is validated at `CREATE` time (broken SQL fails at create, not on
first request). `CREATE OR REPLACE` does not leave a half-applied route on
failure.

### Exec routes (`AS $sql`)

A handler that is exactly one parameter runs the request's SQL text itself —
an HTTP door to the database, without a loopback through another client:

```sql
CREATE AUTH ops AS API_KEY;
CREATE ROUTE sql POST '/sql' REQUIRE ops
  PARAM sql VARCHAR MIN_LENGTH 1
  AS $sql;
```

```sh
curl -X POST http://127.0.0.1:8000/sql -H 'X-API-Key: …' \
  -H 'Content-Type: application/json' \
  -d '{"sql":"CREATE TABLE t AS SELECT 1 AS id; INSERT INTO t VALUES (2); SELECT * FROM t"}'
# [{"id":1},{"id":2}]
```

| Behavior | Detail |
|----------|--------|
| **Recognised** | The handler, trimmed, is one `$name` and nothing else. `CREATE` skips handler validation for it; `$claims_*` never qualifies |
| **Binding** | The parameter binds like any other (JSON body field, form field, query string, `HEADER`/`COOKIE`), so `PARAM … MIN_LENGTH`/`DEFAULT` apply |
| **Execution** | The text is split into statements with DuckDB's parser and each runs once, in order, on the request's connection — DDL, DML, `PRAGMA`, `SET`, `INSTALL`/`LOAD`, `COPY`, `ATTACH` |
| **Response** | The **last** statement's result, through the usual negotiation (JSON default, `Accept: text/csv`, `application/x-ndjson`, `application/vnd.apache.arrow.stream`, route `FORMAT`; `html`/`text` columns). A last statement with no columns (`SET`, `PRAGMA …`) answers `[]` |
| **Errors** | The first failing statement stops the batch — later statements never run — and answers **422** with a FastAPI-shaped `detail` whose `loc` is the parameter (`["body","sql"]`) and whose `msg` is DuckDB's error (`Statement 2 of 3 failed: …` for a batch). Earlier statements are not rolled back unless the batch opened its own `BEGIN` |
| **Limits** | `REQUIRE` runs before any request SQL; `query_timeout_ms` bounds the whole batch (504); `max_response_bytes` applies to the last result |
| **Session** | Each request gets its own connection: `SET`, `TEMP` objects and open transactions end with the request. `SET GLOBAL`, tables and `ATTACH` persist |
| **Policies** | Request SQL cannot be policy-rewritten, so while any row-access or masking policy is bound an exec route answers 403 |

An exec route executes arbitrary SQL with the server's privileges. Keep it on
loopback or behind `REQUIRE`.

#### `quackapi_exec(sql)`: the exec route without HTTP

`quackapi_exec(sql)` returns the receipt `quackapi_post` gives for
`POST {"sql": sql}` to an exec route — same `STRUCT(status, reason, body,
headers, error, reused_connection, body_blob)`, same status codes and bodies — but runs the
SQL on this process's own database, so no HTTP worker is involved:

```sql
-- self-dispatch: one generated statement per row, results as receipts
SELECT list((quackapi_exec('SELECT ' || range || ' AS v')).body) FROM range(3);
-- ['[{"v":0}]', '[{"v":1}]', '[{"v":2}]']
```

A self-post holds a server worker while the SQL it posted posts again, so nested
fan-outs (`array_agg(quackapi_post(...))` inside posted SQL) exhaust a fixed
`worker_threads` pool and wait on each other until their timeouts.
`quackapi_exec` uses a fresh `Connection` per row and DuckDB 2.0's task scheduler.
Nested scalar calls execute inline rather than waiting for capacity in their
own pool. Table calls submit a resumable query and yield the SQL pipeline.
An exec route with no `STATUS`, `EMPTY STATUS`, `ENVELOPE object` or non-JSON
`FORMAT`/`Accept` runs the same code, so the two answer alike.

| Behavior | Detail |
|----------|--------|
| **Execution** | Every statement completes in order on a fresh connection; the last one answers. Scalar batches admit up to 32 native tasks; execution concurrency follows DuckDB's `threads` setting |
| **Threads** | `quackapi_exec_max_threads` (default 256) caps admitted scalar exec tasks across calls, preserving the setting name. It creates no private worker threads. Nested calls, a single engine thread, or exhausted admission run inline. `0` always runs scalar rows inline |
| **Response** | JSON array, `html`/`text` column modes, `location`/`set_cookie` columns as `headers`; `[]` for a final `SET`/`PRAGMA`. No `Accept` negotiation |
| **Errors** | 422 `{"detail":[{"loc":["body","sql"],…}]}` for a failing statement, 504 past the deadline, 507 past `quackapi_max_response_bytes`, 500 for an internal error. `error` is set only when no response could be produced; `NULL` in, `NULL` out |
| **Budget** | Inside a served request (a route, `quackapi_request`), the server's `query_timeout_ms` and `max_response_bytes`, as the exec route uses; elsewhere the session's `quackapi_query_timeout_ms` (default 30 s) and `quackapi_max_response_bytes`. A nested call inherits its caller's budget |
| **Deadline** | One absolute deadline per call, inherited by nested SQL and outbound I/O. Native query execution checks it alongside `max_execution_time`. Caller interruption cancels admitted rows. Blocked native tasks remain owned and are drained before the database closes; inline work returns at its next interrupt point |
| **Headers** | `Content-Type`, `Location`, `Set-Cookie` only — no transport headers (`Content-Length`, `X-Request-ID`); `reused_connection` is `false` |
| **Policies** | While any row access or masking policy is bound, every row answers the exec route's 403 `{"detail":"Policy denies unauthenticated access"}`: neither can rewrite arbitrary SQL to apply a policy, so both fail closed |
| **Not applied** | Route `REQUIRE`, middleware, access log and rate limits: it is a SQL function with the caller's privileges, not a request |
| **Visibility** | The inner connection runs its own transactions: it does not see the caller's uncommitted writes, and what it commits is visible to the caller's next transaction |

The table forms release the pipeline while work is pending:

```sql
-- Native file readers replace the removed GET helper.
SELECT content FROM read_text('https://example.com');
FROM quackapi_post('https://example.com/api', '{"n":42}', content_type := 'application/json');
FROM quackapi_exec('SELECT 42 AS answer');
```

They emit the receipt fields as columns. HTTP tasks run on DuckDB's `async_threads`
pool; submitted SQL advances through native query tasks and completion. The scalar
forms retain row-wise NULL handling and the existing STRUCT result. Native VARIANT
objects and arrays serialize as JSON values rather than display strings. Native
triggers execute in the mutation's own transaction through HTTP and local exec.
The caller's native `max_execution_time` deadline also bounds nested execution
and outbound I/O; an expired native limit raises DuckDB's timeout error.
DuckDB 2.0 currently rejects `RETURNING` on a table with `FOR EACH ROW` triggers;
route creation surfaces that native error before serving requests.

### Scheduled routes

quackapi stores `SCHEDULE` metadata but does not run a scheduler. Timing stays
with the community `cronjob` extension. Scheduled routes must be callable
without path parameters: a `:param` or `{param}` in the final route pattern is
rejected at `CREATE` time. `SCHEDULE` is route-only; `CREATE GROUP` does not
accept it. A route may join a group and schedule its own parameter-free,
group-expanded path.

At application startup, load `cronjob` and register each scheduled route with
its own `cron()` statement. `quackapi_request` executes the route handler
in-process, so no HTTP listener or external request is needed:

```sql
INSTALL cronjob FROM community;
LOAD cronjob;

-- one statement per route
SELECT cron('SELECT * FROM quackapi_request(''POST'', ''/jobs/refresh'')', '0 */5 * * * *');
```

Register one route per `cron()` call. With the `cronjob` extension on DuckDB
1.5.x, a single `SELECT cron(query, schedule) FROM quackapi_routes() WHERE
schedule IS NOT NULL` over several rows registers every job with the **first
row's schedule**, so generate the statements from `quackapi_routes()` and run
them one at a time (self-dispatch):

```sql
SELECT 'SELECT cron(' || chr(39) || 'SELECT * FROM quackapi_request('
    || chr(39) || chr(39) || method || chr(39) || chr(39) || ', '
    || chr(39) || chr(39) || pattern || chr(39) || chr(39) || ')' || chr(39)
    || ', ' || chr(39) || schedule || chr(39) || ');' AS statement
FROM quackapi_routes()
WHERE schedule IS NOT NULL;
```

The optional `test/sql/quackapi_schedule_cronjob.test` SQLLogic case asserts
a single-route registration with Query.Farm's native `cronjob` build when
`QUACKAPI_NETWORK_TESTS=1`.

### Response modes (column names)

| Route mode or column | Response |
|-------------|----------|
| *(default)* | JSON **array of row objects** — types follow DuckDB column types |
| `FORMAT ARROW` or Arrow `Accept` | Arrow IPC stream; `BLOB` columns are byte arrays in Arrow, not a raw file response |
| `FORMAT binary` | Exact `BLOB` response body from one row (`body` plus optional `content_type`) |
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
| `quackapi_serve` | `([port], host := …, static_dir := …, cors_origins := …, memory_limit := …, log_level := …, access_log := …, enable_logging := …, health_routes := …, threads := …, preserve_insertion_order := …, enable_http_metadata_cache := …, worker_threads := …, keep_alive_max_count := …, keep_alive_timeout_sec := …, read_timeout_sec := …, write_timeout_sec := …, compression := …, compression_min_bytes := …, http_client := …, block := …, query_timeout_ms := …, max_response_bytes := …, max_request_bytes := …, cancel_on_disconnect := …, max_pending_requests := …)` | `listen_url` |
| `quackapi_wait` | `(port [, timeout_ms], host := …)` — TCP readiness | `ready`, `listen_url` |
| `quackapi_stop` | `([port])` — omit port to stop all | `status` |
| `quackapi_telemetry_flush` | `([port])` — drain pending sink rows without stopping | `status` |
| `quackapi_routes` | `()` | `name, method, pattern, status, handler, require_auth, group_name, tags, format, envelope, empty_status, timeout_sec, schedule, max_request_bytes` |
| `quackapi_servers` | `()` | `host, port, listen_url, http_client, http_client_reason` |
| Setting | `SET quackapi_cors_origins = '*' \| 'https://a,https://b'` | empty = CORS off |
| Setting | `SET quackapi_memory_limit = '4GB' \| '512MB' \| …` | empty = leave DuckDB `memory_limit` alone |
| Setting | `SET quackapi_log_level = 'silent' \| 'error' \| 'warn' \| 'info' \| 'debug'` | default `info` |
| Setting | `SET quackapi_compression = 'auto' \| 'gzip' \| 'zstd' \| 'off'` | default `auto`; legacy booleans map to `auto`/`off` |
| Setting | `SET quackapi_compression_min_bytes = N` | default `1024` |
| Setting | `SET quackapi_http_client = 'auto' \| 'curl' \| 'httplib'` | auto=prefer+loud fallback; curl=require; httplib=force stock |
| Setting | `SET quackapi_query_timeout_ms = N` | default `30000` |
| Setting | `SET quackapi_http_timeout_ms = N` | default `600000`; maximum outbound call duration absent a shorter outer deadline |
| Setting | `SET quackapi_max_response_bytes = N` | default `16777216` uncompressed bytes |
| Setting | `SET quackapi_max_pending_requests = N` | default `256` |
| Setting | `SET quackapi_exec_max_threads = N` | default `256`; past it `quackapi_exec` rows run on the calling thread; `0` = always |
| Setting | `SET quackapi_graphql_allow_all = true\|false` | default `false`; legacy GraphQL open mode |

`access_log` defaults to structured stderr JSON; `false` disables it and a table
name enables asynchronous table-backed logging. Table writes batch up to 100
rows or one second; `quackapi_telemetry_flush` drains pending rows without
stopping the server. The queue uses 10,000 entries and reports overflow in
`/healthz` as `access_log_overflow_count`. Compression defaults to `auto`,
negotiates client q-values with zstd winning ties, skips bodies below 1024 bytes,
and adds `Vary: Accept-Encoding` to responses eligible for compression.

HTTP concurrency is server-wide: by default up to 32 requests are active at
once (`worker_threads`), and up to 256 accepted connections can wait for a
worker (`max_pending_requests`). These are configurable server-wide limits,
not per-route limits; all routes share the same pool. Requests
that arrive after the pending queue fills are closed at the connection
admission layer, without a guaranteed HTTP error response. This is separate
from DuckDB's `threads` setting, which controls query execution.

Access-log headers are opt-in through the `log_headers` allowlist (empty by
default). Sensitive names such as `Authorization`, `Cookie`, `Set-Cookie`,
`X-API-Key`, and names containing `token`, `secret`, or `password` remain
redacted even if allowlisted. Sensitive query keys are redacted when query
logging is enabled. Avoid putting credentials in request bodies or paths, and
only allowlist headers needed for operations.

Built-in OpenAPI (not listed in `quackapi_routes()`):

- `GET /openapi.json` — OpenAPI 3.1 from the live registry  
- `GET /docs` — Swagger UI  
- `GET /redoc` — ReDoc  

**Database-wide settings.** `memory_limit` and `preserve_insertion_order` apply
to every connection in the database, not only to HTTP handlers, so
`quackapi_serve` changes them only when asked:

1. `memory_limit := '…'` named parameter on `quackapi_serve` wins  
2. else `SET quackapi_memory_limit = '…'`  
3. else DuckDB's `memory_limit` is left as it is
4. `preserve_insertion_order := true|false` sets it; omitted, it is left as it is

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

Default request body limit: **8 MiB** (`413` when exceeded).
For a larger image upload, raise both the route limit and server ceiling; the
effective cap is the smaller value:

```sql
CREATE ROUTE image_upload POST '/images' WITH (max_request_bytes := 33554432) AS
  SELECT octet_length($file::BLOB) AS bytes;

SELECT * FROM quackapi_serve(8000, max_request_bytes := 33554432);
```

Here `$file` is the multipart file part. The route option is still capped by
the server option; raising the server ceiling alone leaves routes at the 8 MiB
default. See the [request body guide](docs/guide/request-bodies.md) for the full
limit semantics.

### Auth

```sql
CREATE [OR REPLACE] AUTH <name> AS API_KEY [( HEADER 'X-Custom-Key' )];
CREATE [OR REPLACE] AUTH <name> AS JWT ( SECRET '…' [, ALGORITHM HS256] );
DROP AUTH <name>;

SELECT * FROM quackapi_add_api_key('site', 'raw-key', 'alice');  -- stores SHA-256 only
SELECT * FROM quackapi_auths();   -- name, kind, header — never secrets/hashes
```

Verified JWT/API-key claims bind as `$claims_<name>` (missing claim → SQL `NULL`,
not 422). REST credentials are verified with `quackapi_verify_auth`. Core Quack manages
its own RPC authentication and authorization settings; serving REST does not
override them.

### Route groups (APIRouter-style)

```sql
CREATE GROUP v1 WITH (prefix='/api/v1', auth=api, tags='items,v1');
CREATE ROUTE items_list GET '/items' GROUP v1 AS SELECT …;
-- served at GET /api/v1/items, inherits auth + tags
SELECT * FROM quackapi_groups();
```

### PostgreSQL through DuckDB

```sql
INSTALL postgres;
LOAD postgres;
ATTACH 'host=127.0.0.1 dbname=mydb' AS pg (TYPE postgres);
CREATE ROUTE orders GET '/orders/:id' AS
  SELECT * FROM pg.public.orders WHERE id = $id::BIGINT;
```

Configure credentials using DuckDB secrets or PostgreSQL environment variables.
The former quackapi `pg_dsn` option and `quackapi_pg_dsn` setting are retired:
nonempty values now fail explicitly. Attach the database with the PostgreSQL
extension and use the attached catalog in route SQL. PostgreSQL-specific SQL can
also use the extension's `postgres_query` and `postgres_execute` functions.

### Table API (read-only scaffold)

```sql
CREATE API FOR TABLE documents [AT '/docs'] [KEY 'id'];
-- GET /docs?limit=100&offset=0 (maximum limit 1000)
-- registers GET /docs and GET /docs/:id only (no write routes)
```

### Durable job queue (broker-less)

Jobs live in the ordinary table `quackapi_jobs` in your `.db` file — no Redis.

```sql
CREATE QUEUE emails WITH (max_attempts=5, visibility_timeout='30s');

CREATE ROUTE enqueue POST '/jobs' STATUS 201 PARAM payload VARCHAR AS
SELECT quackapi_enqueue('emails', $payload) AS job_id;

SELECT id, payload, quackapi_ack('emails', id, delivery_generation) AS acked
FROM quackapi_dequeue('emails', 10);
```

| Function | Role |
|----------|------|
| `quackapi_enqueue(queue, payload [, max_attempts])` | → `job_id` |
| `quackapi_dequeue(queue [, n])` | claim with a visibility lease and delivery generation |
| `quackapi_ack` / `quackapi_nack` / `quackapi_renew` | complete, retry, or renew the matching delivery |
| `quackapi_queues()` | depth, in_flight, dead, options |

Worker = compose community `cronjob` (or a drain route). See [`docs/QUEUE.md`](docs/QUEUE.md).

### Arrow responses and binary data

A route can return an Arrow IPC stream, including `BLOB` values as binary data:

```sql
CREATE ROUTE image_rows GET '/images' FORMAT ARROW AS
  SELECT image_bytes AS png FROM generated_images;
```

Alternatively, send `Accept: application/vnd.apache.arrow.stream` to a JSON-format
route. The response is an Arrow stream, not a raw PNG: an Arrow client must decode
it to obtain the bytes. `FORMAT ARROW` passes native DuckDB result batches to the
vendored nanoarrow encoder without rebuilding rows. The HTTP body is still
buffered, and the response cap is checked after each encoded batch. JSON rows
are encoded once while fetching results. Run [the large-response benchmark](bench/large_response.sql)
with this checkout's CLI to measure both paths. See
[`docs/reference/ddl.md`](docs/reference/ddl.md) for negotiation and precedence.
For a direct PNG response, use `FORMAT binary` and return one row with a BLOB
named `body` and an optional `content_type`. DuckDB 2.0's `read_blob` reads a
file as a `BLOB`, including remote files supported by the active filesystem:

```sql
CREATE ROUTE image GET '/image/:id' FORMAT binary AS
  SELECT content AS body, 'image/png' AS content_type
  FROM read_blob('/srv/images/' || $id::VARCHAR || '.png');
```

The response body is the exact BLOB bytes. Omitting `content_type` uses
`application/octet-stream`; binary responses require exactly one row.
For an upstream HTTP response where status and headers matter, load the
community `http_request` extension and use its `http_get(...)` response fields;
its `body` is a `BLOB`. `read_blob(url)` is simpler when only the bytes are
needed. The removed `quackapi_fetch` is not needed for either case.

This revision targets DuckDB **2.0.0**, not 1.5.5. Use an extension build matching
the running host; see [`SUPPORTED_HOSTS.md`](SUPPORTED_HOSTS.md).

### SSE and WebSocket streams

```sql
CREATE STREAM ticks GET '/events' AS SELECT …;   -- text/event-stream
CREATE STREAM chat WS '/chat' AS SELECT upper($message) AS said;   -- one JSON frame per row
SELECT * FROM quackapi_streams();
```

See [`docs/guide/stream.md`](docs/guide/stream.md).

An interval stream can report progress by polling a persisted status row while
separate work updates it:

```sql
CREATE STREAM job_progress GET '/jobs/:job_id/events' WITH (interval='500ms') AS
SELECT status, percent, updated_at
FROM job_status
WHERE job_id = $job_id::BIGINT;
```

This polls DuckDB state; `quackapi_post` waits for the complete upstream
response and does not stream upstream tokens. Start long work independently
(for example, through a queue worker) and update `job_status` as it proceeds.

### Row-access & masking policies

```sql
CREATE ROW ACCESS POLICY tenant_isolation AS (tenant_id) RETURNS BOOLEAN
  USING (tenant_id = $claims_tenant);
CREATE MASKING POLICY mask_email ON VARCHAR USING (…);
ALTER TABLE … ADD ROW ACCESS POLICY …;
SELECT * FROM quackapi_policies();
```

### Outbound HTTP

A handler that calls an upstream API — an LLM, a payment gateway, a webhook —
gets a **pooled** connection. The pool lives for the process, so the second
request to a host does not re-dial.

```sql
CREATE ROUTE ask POST '/ask' AS
  SELECT (quackapi_post(
    'http://127.0.0.1:11434/api/generate',
    to_json({model: $model::VARCHAR, prompt: $prompt::VARCHAR, stream: false})::VARCHAR
  )).body AS answer;
```

| Function | Role |
|----------|------|
| `quackapi_post(url, body[, content_type[, headers]], timeout_ms := …)` | Scalar or table POST (`content_type` defaults to `application/json`); both forms return exact `body_blob` bytes |
| `quackapi_http_pool()` | per-origin `host, idle, dialed, reused` |
| `quackapi_exec(sql)` | the receipt `quackapi_post` gives from an exec route, computed in-process — see [`quackapi_exec`](#quackapi_execsql-the-exec-route-without-http) |

The pool retains at most 64 idle clients per origin, 256 idle clients overall,
and 256 origin entries; overflow clients are not retained for reuse.

The plain-HTTP client keeps connections across route executions. An earlier
benchmark against an Ollama endpoint measured **39.1ms p50** without pooling
and **13.2ms p50 / 13.8ms p90** with it. See
[`docs/EXTENSION_OVERLAP.md`](docs/EXTENSION_OVERLAP.md) for a comparison with
DuckDB's remote readers and the community `http_request` extension.

`http://` uses the vendored httplib client, which implements POST — no companion
extension needed. `https://` goes through `HTTPUtil`, pooled the same way, so
`LOAD curl_httpfs` (or `httpfs`) supplies TLS.

Enable `quackapi_serve(..., cancel_on_disconnect := true)` to interrupt ordinary
route SQL when the downstream socket closes and abort a blocked plain-HTTP POST.
This is opt-in because socket EOF also includes a client deliberately half-closing
its write side. The HTTPS `HTTPUtil` interface has no portable abort hook, so a
blocked HTTPS call remains bounded by its configured timeout.

Per-call outbound timeouts cap only the HTTP wait. They do not extend the
enclosing query or route's `query_timeout_ms`; the earlier deadline wins. A
client disconnect closes the response path, but does not guarantee that an
already-submitted upstream job is cancelled. Call the upstream cancellation
API when stopping remote work is required.

Pass a keyword-only timeout in milliseconds to either the scalar or table form.
An explicit value overrides `quackapi_http_timeout_ms` for that call, but still
cannot exceed the outer deadline:

```sql
SELECT (quackapi_post('https://api.example.com/jobs', $body::VARCHAR,
                      timeout_ms := 120000)).body_blob;

SELECT status, body_blob
FROM quackapi_post('https://api.example.com/jobs', '{"run":true}',
                   content_type := 'application/json', timeout_ms := 120000);
```

`body_blob` always preserves the response bytes. The compatibility `body`
field is `NULL` when those bytes are not valid UTF-8; otherwise it contains the
same response decoded as text.

Both are `VOLATILE`: DuckDB constant-folds a literal-argument call otherwise, and
`FROM range(1000)` would issue one request while reporting 1000 rows.

### Framework importers

```sql
SELECT * FROM quack_from_routes('fastapi', '/path/to/app');
SELECT * FROM quack_from_models('fastapi', '/path/to/app');
```

Both functions accept `fastapi`, `rails`, `express`, or `gin`. They replace the
framework-specific function names. See [the importer guide](bridges/from_x/README.md)
for the route and model schemas.

### Diagnostics

For the companion HTTP backend, load `curl_httpfs` and call its native
`curl_httpfs_http_util_name()`. Quackapi's selected backend and fallback reason
are also reported by `quackapi_servers()` and `/healthz`.

Outbound HTTPS uses DuckDB’s shared `HTTPUtil` (no libcurl linked into quackapi).
`quackapi_serve` **batteries prefer `curl_httpfs`** (pool + HTTP/2 + async).
`http_client := 'auto'` falls back to httplib with a loud `http_client_reason` on
`/healthz` / `quackapi_servers()`; `http_client := 'curl'` **fails serve** if
curl_httpfs cannot INSTALL/LOAD — see [`docs/curl_httpfs.md`](docs/curl_httpfs.md).

---

## Install & load

### Community (preferred — signed)

Page: [duckdb.org/community_extensions/extensions/quackapi](https://duckdb.org/community_extensions/extensions/quackapi)

```sql
INSTALL quackapi FROM community;
LOAD quackapi;
```

| Requirement | Detail |
|-------------|--------|
| **DuckDB** | **v2.0.0** |
| **CI / platforms** | linux_amd64, linux_arm64, osx_amd64, osx_arm64, windows_amd64 |
| **Community CDN** | `…/v2.0.0/{platform}/quackapi.duckdb_extension.gz` |
| **Wasm** | excluded (no server sockets) |

See [`SUPPORTED_HOSTS.md`](SUPPORTED_HOSTS.md).

### Direct download (offline / unsigned)

Release assets: https://github.com/asubbarao/quackapi/releases  

```text
quackapi-duckdb-v2.0.0-linux_amd64.duckdb_extension
quackapi-duckdb-v2.0.0-windows_amd64.duckdb_extension
quackapi-duckdb-v2.0.0-osx_arm64.duckdb_extension
…
```

```sh
curl -fsSL -o quackapi.duckdb_extension \
  "https://github.com/asubbarao/quackapi/releases/download/<tag>/quackapi-duckdb-v2.0.0-osx_arm64.duckdb_extension"
duckdb -unsigned -c "LOAD '$(pwd)/quackapi.duckdb_extension';"
```

### From source

```sh
git clone --recurse-submodules https://github.com/asubbarao/quackapi
cd quackapi
GEN=ninja make release
./build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/quackapi/quackapi.duckdb_extension';
```

**Target DuckDB:** **v2.0.0.** Dependencies: DuckDB’s bundled
**httplib** + **mbedtls** only — no vcpkg, no libcurl. The extension is built as
C++17 by the repository CMake configuration.

---

## Behavior notes

- **Response envelope:** default is a JSON **array** of row objects for
  multi-column / non-`html`/`text` handlers. Opt in with `ENVELOPE object`
  for a bare JSON object (exactly one row) and `EMPTY STATUS 404` for 0-row
  → HTTP 404 instead of `[]` / `null`.
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
  in with `host := '0.0.0.0'`.
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
- **Platform coverage:** **DuckDB v2.0.0** on linux/macOS/windows_amd64 only.
  Older DuckDB versions are unsupported — upgrade the host.
- **Serve memory limit:** `quackapi_serve` sets no `memory_limit` of its own;
  the database keeps DuckDB's default (80% of RAM) unless you pass
  `memory_limit := '…'`, `SET quackapi_memory_limit`, or `SET memory_limit`.
- **WebSockets are streams, not routes:** `CREATE STREAM … WS` pushes rows or
  answers each client message; there is no `REQUIRE` auth or middleware on them yet.
- **`CREATE API FOR TABLE`** scaffolds **GET list + GET by key** only.
- **JWT:** HS256 only (`ALGORITHM HS256`); no RS256 / OIDC discovery yet.
- **Not a general multi-tenant SaaS framework:** policies are claims-keyed
  helpers, not a full RBAC product.

---

## Build & test

```sh
GEN=ninja make release
make test                          # SQL unit tests
bash test/conformance/run.sh       # real-HTTP contract suite (needs release build)
```

## Docs in this repo

| Doc | Content |
|-----|---------|
| [`docs/index.md`](docs/index.md) | Guided tour (start here) |
| [`docs/guide/extension-composition.md`](docs/guide/extension-composition.md) | Compose companions: gateway, SSE, `quack_from_x`, mesh, PDF |
| [`docs/community-page.md`](docs/community-page.md) | Community-extensions page copy |
| [`packaging/description.yml`](packaging/description.yml) / [`description.yml`](description.yml) | Community submission manifest |
| [`docs/FASTAPI_PARITY.md`](docs/FASTAPI_PARITY.md) | Scorecard vs FastAPI |
| [`docs/SUPERIORITY_ORDER.md`](docs/SUPERIORITY_ORDER.md) | Equivalence → slaughter → leapfrog (work order) |
| [`docs/QUEUE.md`](docs/QUEUE.md) | Job queue semantics |
| [`docs/PYDANTIC_PARITY.md`](docs/PYDANTIC_PARITY.md) | Native typed bodies, validation errors, nulls, and OpenAPI |
| [`docs/MIDDLEWARE.md`](docs/MIDDLEWARE.md) | SQL request and response middleware |
| [`docs/RESOURCE_LIMITS.md`](docs/RESOURCE_LIMITS.md) | Query deadlines, response caps, admission, and rate limits |
| [`docs/REVIEW_2026_09_07.md`](docs/REVIEW_2026_09_07.md) | Security and robustness review with remediation status |
| [`docs/curl_httpfs.md`](docs/curl_httpfs.md) | Outbound HTTP via curl_httpfs |

## License

MIT — Copyright (c) 2026 Alok Subbarao
