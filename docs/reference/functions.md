# Function reference — every `quackapi_*`

Authoritative list from [FEATURE_STATUS §1.4](../FEATURE_STATUS.md) (live registry). Signatures from `src/` and versioned tests. One-line examples were run against `build/release/duckdb -unsigned`.

---

## Server lifecycle

### `quackapi_serve([port], host := …, static_dir := …, cors_origins := …, memory_limit := …, log_level := …, slow_request_ms := …, log_headers := …, log_query := …, request_ring := …, access_log := …, enable_logging := …, health_routes := …, threads := …, preserve_insertion_order := …, enable_http_metadata_cache := …, worker_threads := …, keep_alive_max_count := …, keep_alive_timeout_sec := …, read_timeout_sec := …, write_timeout_sec := …, compression := …, compression_min_bytes := …, http_client := …, block := …, query_timeout_ms := …, max_response_bytes := …, max_request_bytes := …, cancel_on_disconnect := …, max_pending_requests := …)`

The former `pg_dsn` / `quackapi_pg_dsn` native adapter is removed. Nonempty
legacy settings fail explicitly; use DuckDB's `postgres` extension and an
attached catalog instead.


| | |
|--|--|
| **Kind** | Table function |
| **Args** | `port INTEGER` optional (default in implementation if omitted — prefer passing explicitly, e.g. `8000`) |
| **Named** | `host VARCHAR` (default `127.0.0.1`), `static_dir VARCHAR` (default empty), `cors_origins VARCHAR` (default empty), `memory_limit VARCHAR` (default empty), `log_level VARCHAR` (default `info`), `slow_request_ms BIGINT` (default `1000`; at `warn`, requests this slow are logged), `log_headers VARCHAR` (default empty; comma-separated header allowlist), `log_query BOOLEAN` (default `false`; redacted raw query), `access_log ANY` (default `true`: stderr JSON; `false`: off; table name: table-backed), `enable_logging BOOLEAN` (default `false`), `health_routes BOOLEAN` (default `true`), `threads VARCHAR` (default empty), `preserve_insertion_order BOOLEAN` (omitted: left as it is), `enable_http_metadata_cache BOOLEAN` (default `true`), `worker_threads INTEGER` (default `32`), `keep_alive_max_count INTEGER` (default `128`), `keep_alive_timeout_sec INTEGER` (default `10`), `read_timeout_sec INTEGER` (default `30`), `write_timeout_sec INTEGER` (default `30`), `compression ANY` (default `auto`; `true`/`false` map to `auto`/`off`), `compression_min_bytes BIGINT` (default `1024`), `http_client VARCHAR` (default `auto`; `auto`\|`curl`\|`httplib`), `block BOOLEAN` (default `false`), `query_timeout_ms BIGINT` (default `30000`), `max_response_bytes BIGINT` (default `16777216`), `max_request_bytes BIGINT` (default `8388608`, maximum `1073741824`; server hard ceiling), `cancel_on_disconnect BOOLEAN` (default `false`), `max_pending_requests BIGINT` (default `256`) |
| **Returns** | `listen_url VARCHAR` |

```sql
SELECT * FROM quackapi_serve(8000);
-- http://127.0.0.1:8000

SELECT * FROM quackapi_serve(
  8000,
  host := '127.0.0.1',
  static_dir := './static',
  cors_origins := '*',
  memory_limit := '4GB',
  http_client := 'auto'   -- prefer curl_httpfs; fall back to httplib
);

-- Supervised process (launchd / KeepAlive): hold until quackapi_stop or SIGINT/SIGTERM.
-- listen_url is still emitted first; the query stays open afterward.
SELECT * FROM quackapi_serve(8000, block := true);
```

**`block`:** default `false` — serve returns immediately after bind (backward compatible).
With `block := true`, the query emits `listen_url` then waits until `quackapi_stop`
(or SIGINT/SIGTERM / query interrupt). Prefer this over shell `sleep` / `lsof` keepalive loops.

**Memory limit precedence:** named param → `SET quackapi_memory_limit` → else DuckDB's `memory_limit` is left as it is. `preserve_insertion_order` is set only when the named parameter is given.

**Outbound HTTP client:** default `auto` INSTALL/LOADs community `curl_httpfs` and sets
`httpfs_client_implementation='curl'` (connection pool, HTTP/2, async). If unavailable
(Windows/WASM/offline), logs `quackapi.http_client=httplib reason=curl_httpfs_unavailable`
and continues. Override with `http_client := 'httplib'` or `SET quackapi_http_client`.
Inbound server remains httplib. See [curl_httpfs.md](../curl_httpfs.md).

**Access log:** a table destination is written asynchronously in batches of up to
100 rows or one second. `quackapi_telemetry_flush` drains pending rows without
stopping the server. The queue holds 10,000 entries; overflow is written to
stderr and counted as `access_log_overflow_count` in `/healthz`. A failed table
flush falls back to stderr, retries with 1-second-to-60-second backoff, emits one
warning per failure episode, and logs a recovery line when the table works again.

`request_ring BIGINT` defaults to `10000` (maximum `1000000`); `0` disables the in-memory ring.

### `quackapi_requests([port])`

| | |
|--|--|
| **Kind** | Table function |
| **Args** | `port INTEGER` optional; required when multiple servers are running |
| **Returns** | `request_id`, `received_at`, `method`, `path`, `route_name`, `route_path`, `status`, `duration_ms`, `sql_prepare_ms`, `sql_execute_ms`, `rows_out`, `bytes_in`, `bytes_out`, `client_ip`, `user_agent`, `http_version`, `error_type`, `error_message`, `trace_id`, `span_id`, `parent_span_id`, `sampled`, `headers`, `query` |

Returns recent TCP requests oldest-first from a bounded in-memory ring. It
returns no rows when no server is running; unknown optional fields are NULL.
The ring is independent of `access_log`, so it records requests even when
access-log output is disabled or filtered.

```sql
FROM quackapi_requests() WHERE status >= 500;
FROM quackapi_requests(8000) ORDER BY received_at;
```

### `quackapi_telemetry_status([port])`

| | |
|--|--|
| **Kind** | Table function |
| **Args** | `port INTEGER` optional; required when multiple servers are running |
| **Returns** | `sink`, `target`, `queued`, `exported_total`, `dropped_total`, `last_error`, `last_export_age_ms` |

Returns one row per configured telemetry sink. It returns no rows when no server
is running. `target` is the table name for the table sink and empty for stderr;
`dropped_total` counts table-queue overflow.

### `quackapi_telemetry_flush([port])`

Drains pending telemetry rows for the selected running server without stopping
it. This is a synchronous completion barrier for asynchronous table sinks; a
port is required when multiple servers are running.

**Compression:** `auto` negotiates `Accept-Encoding` q-values, preferring zstd on
ties. `gzip` and `zstd` restrict the selected coding, `off` disables compression,
and bodies smaller than `compression_min_bytes` are skipped. Eligible responses
advertise `Vary: Accept-Encoding`.

---

### `quackapi_wait(port [, timeout_ms], host := …)`

| | |
|--|--|
| **Kind** | Table function |
| **Args** | `port INTEGER` required; `timeout_ms BIGINT` optional (omit / negative = wait until ready or interrupt) |
| **Named** | `host VARCHAR` (default `127.0.0.1`), `timeout_ms BIGINT` |
| **Returns** | `ready BOOLEAN`, `listen_url VARCHAR` (NULL when not ready) |

TCP readiness probe — returns once `host:port` accepts connections (any HTTP status,
including 404). Replaces `sleep` / `lsof` / `curl` boot loops.

```sql
SELECT * FROM quackapi_serve(8000);
SELECT ready, listen_url FROM quackapi_wait(8000, 5000);
-- true | http://127.0.0.1:8000

-- Timed miss (dead port)
SELECT ready, listen_url FROM quackapi_wait(59999, 100);
-- false | NULL
```

---

### `quackapi_stop([port])`

| | |
|--|--|
| **Kind** | Table function |
| **Args** | `port INTEGER` optional — omit to stop **all** servers |
| **Returns** | `status VARCHAR` |

```sql
SELECT * FROM quackapi_stop(8000);
-- Stopped quackapi server on port 8000

SELECT * FROM quackapi_stop();
-- Stopped all quackapi servers
```

---

### `quackapi_servers()`

| | |
|--|--|
| **Kind** | Table function |
| **Returns** | `host`, `port`, `listen_url`, `http_client` (`curl` or `httplib`), `http_client_reason` |

```sql
SELECT * FROM quackapi_servers();
```

---

## Registry inspection

### `quackapi_routes()`

| | |
|--|--|
| **Returns** | `name`, `method`, `pattern`, `status`, `handler`, `require_auth`, `group_name`, `tags`, `format`, `envelope`, `empty_status`, `timeout_sec`, `schedule`, `max_request_bytes` |

```sql
SELECT name, method, pattern FROM quackapi_routes();
```

---

### `quackapi_auths()`

| | |
|--|--|
| **Returns** | `name`, `kind`, `header` — **never** secrets or key hashes |

```sql
SELECT name, kind, header FROM quackapi_auths();
-- site | API_KEY | X-API-Key
```

---

### `quackapi_groups()`

| | |
|--|--|
| **Returns** | `name`, `prefix`, `require_auth`, `tags`, `members` |

```sql
SELECT name, prefix, members FROM quackapi_groups();
```

---

### `quackapi_queues()`

| | |
|--|--|
| **Returns** | `name`, `depth`, `in_flight`, `dead`, `max_attempts`, `visibility_timeout_sec`, `backoff_base_sec` |

```sql
SELECT name, depth, in_flight, dead FROM quackapi_queues();
```

---

### `quackapi_streams()`

| | |
|--|--|
| **Returns** | `name`, `method`, `pattern`, `transport`, `interval_ms`, `handler` |

```sql
SELECT name, pattern, transport, interval_ms FROM quackapi_streams();
```

---

### `quackapi_policies()`

| | |
|--|--|
| **Returns** | `name`, `kind`, `signature`, `expression`, `bound_table`, `bound_columns` |

```sql
SELECT name, kind, bound_table FROM quackapi_policies();
```

---

## Auth helpers

### `quackapi_add_api_key(auth_name, raw_key, subject)`

| | |
|--|--|
| **Kind** | Table function |
| **Args** | three `VARCHAR` |
| **Returns** | `subject VARCHAR` |
| **Side effect** | Stores **SHA-256** of `raw_key` under the API_KEY scheme |

```sql
SELECT * FROM quackapi_add_api_key('site', 'k-secret', 'alice');
-- alice
```

Errors if scheme missing or not `API_KEY`.

---

### `quackapi_verify_auth(scheme, auth_string)`

| | |
|--|--|
| **Kind** | Scalar → struct |
| **Args** | `scheme VARCHAR`, `auth_string VARCHAR` |
| **Returns** | struct with at least `ok BOOLEAN`, `status INTEGER`, `claims_json VARCHAR` |

```sql
SELECT (quackapi_verify_auth('site', 'k-secret')).ok;     -- true
SELECT (quackapi_verify_auth('site', 'wrong')).status;    -- 401
```

---

## Queue

### `quackapi_enqueue(queue, payload [, max_attempts])`

| | |
|--|--|
| **Kind** | Scalar |
| **Args** | `queue VARCHAR`, `payload VARCHAR` or `JSON`, optional `max_attempts INTEGER` |
| **Returns** | `job_id BIGINT` |

```sql
SELECT quackapi_enqueue('default', '{"task":"email"}');
-- 1
```

---

### `quackapi_dequeue(queue [, n])`

| | |
|--|--|
| **Kind** | Table function |
| **Args** | `queue VARCHAR`, optional `n INTEGER` (default 1, max 1000) |
| **Returns** | `id`, `queue`, `payload`, `status`, `attempts`, `max_attempts`, `visible_at`, `last_error`, `delivery_generation` |

```sql
SELECT id, payload, status FROM quackapi_dequeue('default', 10);
```

---

### `quackapi_ack(queue, job_id, delivery_generation)`

| | |
|--|--|
| **Kind** | Scalar |
| **Args** | `queue VARCHAR`, `job_id BIGINT`, `delivery_generation BIGINT` |
| **Returns** | `BOOLEAN` — true if job was `running` and marked `done` |

```sql
SELECT quackapi_ack('default', 1, 1);  -- true when generation 1 is still leased
```

---

### `quackapi_nack(queue, job_id, delivery_generation [, requeue [, error]])`

| | |
|--|--|
| **Kind** | Scalar |
| **Args** | `queue VARCHAR`, `job_id BIGINT`, `delivery_generation BIGINT`, optional `requeue BOOLEAN` (default true), optional `error VARCHAR` |
| **Returns** | `VARCHAR` new status (`pending` or `dead`) |

```sql
SELECT quackapi_nack('default', 1, 1, true, 'try_again');  -- pending or dead
SELECT quackapi_nack('default', 1, 1, false, 'no_retry');  -- dead
```

---

### `quackapi_renew(queue, job_id, delivery_generation)`

| | |
|--|--|
| **Kind** | Scalar |
| **Args** | `queue VARCHAR`, `job_id BIGINT`, `delivery_generation BIGINT` |
| **Returns** | `BOOLEAN` — true when the active lease was renewed |

```sql
SELECT quackapi_renew('default', 1, 1);
```

---

## Durable table

### `quackapi_jobs`

Created on first `CREATE QUEUE`. Ordinary catalog table:

| Column | Role |
|--------|------|
| `id` | Job id |
| `queue` | Queue name |
| `payload` | VARCHAR JSON text |
| `status` | `pending` / `running` / `done` / `dead` |
| `attempts`, `max_attempts` | Retry bookkeeping |
| `visible_at` | Lease / backoff timestamp |
| `last_error` | Last nack message |

```sql
SELECT id, payload, status FROM quackapi_jobs WHERE status = 'done';
```

---

## Outbound POST and local execution

### `quackapi_post(url, body[, content_type[, headers]], response_format := 'raw', timeout_ms := …)`

Both scalar and table forms return `status, reason, body, headers, error,
reused_connection, body_blob, columns, rows`. `body_blob` preserves exact bytes;
`body` is NULL for invalid UTF-8. `columns` is NULL unless `response_format`
requests native decoding, and is then a `LIST<STRUCT(name VARCHAR, type TYPE)>`
with DuckDB's native logical types (for example, display a type as text with
`columns[1].type::VARCHAR`). `rows` is likewise NULL unless native
decoding is requested, and is then a `LIST<VARIANT>` that preserves DuckDB types.
Successful empty results retain their schema in `columns` and have an empty
`rows` list. The default
format is `raw`; `arrow` decodes Arrow IPC streams and `parquet` decodes Parquet
responses. These modes set the matching `Accept` header. A supplied header must
allow that media type with positive quality, respecting more specific refusals;
compatible wildcard or weighted headers are narrowed to the selected format.
They decode only successful
responses with the matching `Content-Type`; a decode failure is recorded in
`error` while the original status and `body_blob` remain available. The body and
native serialized `{columns, rows}` result are each limited by
`quackapi_max_response_bytes`. Parquet
decoding uses a uniquely named DuckDB temporary file because the native reader
accepts paths; it is removed after each decode. The keyword-only positive
`timeout_ms` overrides `quackapi_http_timeout_ms` for one call without extending
the enclosing query deadline. NULL input produces a NULL scalar receipt.

```sql
SELECT status, body_blob
FROM quackapi_post('https://api.example.com/jobs', '{"run":true}',
                  content_type := 'application/json', timeout_ms := 120000);
```

### `quackapi_http_pool()`

Reports `host, idle, dialed, reused` per origin. Retention is bounded to 64 idle
clients per origin, 256 idle clients overall, and 256 origin entries.

### `quackapi_selfdispatch(sql)`

Scalar/table execution of a SQL batch on a fresh local connection, returning
`status, reason, body, headers, error, reused_connection, body_blob, columns, rows`.
`columns` is `LIST<STRUCT(name VARCHAR, type TYPE)>` and retains native names and
logical types for successful results, including
empty results; `rows` contains native `VARIANT` row objects. Statements run in order and the final result
answers. Errors stop the batch; earlier statements are not automatically rolled
back. Policy-bound databases fail closed with 403. Caller deadlines and response
budgets propagate into nested execution; NULL scalar input produces NULL.

This runs with the SQL caller's privileges and does not apply route auth,
middleware, rate limits, or access logging. See the [README](../../README.md#quackapi_selfdispatchsql-the-exec-route-without-http)
for execution scheduling, visibility and error details.

---

## Other registered functions

These public functions are registered by the extension in addition to the
server, registry, auth, queue, and outbound HTTP surfaces above.

| Function | Signature / returns |
|----------|--------------------|
| `quackapi_request` | `quackapi_request(method VARCHAR, path VARCHAR [, body VARCHAR], headers := MAP, access_log := ANY, log_headers := VARCHAR, log_query := BOOLEAN, query_timeout_ms := BIGINT, max_response_bytes := BIGINT)` → `status INTEGER, body BLOB, content_type VARCHAR, headers MAP(VARCHAR, VARCHAR)`; no TCP listener is required |
| `quackapi_middlewares()` | → `name, phase, group_name, handler_sql, registration_order` |
| `quackapi_graphql_tables()` | → `mode, table_name` |
| `quackapi_graphql_routes()` | → `name, method, path, tables, require_auth, limit` |
| `quack_from_routes(framework VARCHAR, path VARCHAR)` | → `method, path, handler_name, file, start_line, evidence` |
| `quack_from_models(framework VARCHAR, path VARCHAR)` | → `model_name, field_name, field_type, is_required, is_optional, has_default, default_expr, file, field_line` |

`quackapi_apply_*` planner functions are internal DDL implementation helpers,
not application-facing functions.

---

## Settings

| Setting | Meaning |
|---------|---------|
| `SET quackapi_cors_origins = '*' \| 'https://a,https://b'` | CORS allow list; empty = off |
| `SET quackapi_memory_limit = '4GB' \| '512MB' \| …` | Serve memory preference when named param omitted |
| `SET quackapi_log_level = 'silent' \| 'error' \| 'warn' \| 'info' \| 'debug'` | Serve log verbosity; default `info` |
| `SET quackapi_request_ring = N` | Recent TCP requests retained by `quackapi_requests()`; default `10000`, maximum `1000000`, `0` disables |
| `SET quackapi_log_headers = 'authorization,x-tenant-id'` | Access-log request-header allowlist; default empty and denylisted values are redacted |
| `SET quackapi_log_query = true\|false` | Preserve a redacted raw query in access logs; default `false` |
| `SET quackapi_compression = 'auto' \| 'gzip' \| 'zstd' \| 'off'` | Response compression; default `auto` |
| `SET quackapi_compression_min_bytes = N` | Minimum response size; default `1024` |
| `SET quackapi_http_client = 'auto' \| 'curl' \| 'httplib'` | Outbound httpfs client preference (default `auto` → curl_httpfs) |
| `SET quackapi_http_timeout_ms = N` | Default per-call outbound POST timeout; default `600000` ms, with shorter outer deadlines taking precedence |
| `SET quackapi_pg_dsn = '…'` | Retired; a nonempty value triggers a migration error. Use `ATTACH … (TYPE postgres)`. |
| `SET quackapi_query_timeout_ms = N` | Query execution budget; default `30000` |
| `SET quackapi_max_response_bytes = N` | Uncompressed response cap; default `16777216` |
| `SET quackapi_max_request_bytes = N` | Server upload ceiling; default `8388608`, maximum `1073741824`; routes without an explicit override still use 8 MiB |
| `SET quackapi_max_pending_requests = N` | Pending request cap; default `256` |
| `SET quackapi_selfdispatch_max_threads = N` | Process-wide self-dispatch task admission cap (no private worker threads); exhausted admission runs inline; default `256`, `0` always inline |
| `SET quackapi_graphql_allow_all = true\|false` | Legacy global GraphQL open mode; default `false` |

```sql
SET quackapi_cors_origins = '*';
SET quackapi_memory_limit = '4GB';
SET quackapi_http_client = 'auto';
SET quackapi_compression = 'auto';
SET quackapi_compression_min_bytes = 1024;
```

---

## Built-in HTTP paths (not functions)

Served automatically when any `quackapi_serve` is listening:

| Path | Role |
|------|------|
| `GET /openapi.json` | OpenAPI 3.1 |
| `GET /docs` | Swagger UI |
| `GET /redoc` | ReDoc |

See [OpenAPI guide](../guide/openapi.md).
