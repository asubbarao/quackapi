# Function reference

This page documents the SQL functions in the current QuackAPI build. Removed
experimental APIs and their migration paths are listed in the
[migration guide](../MIGRATION.md).

## Server lifecycle

### `quackapi_serve([port], ...)`

Starts an HTTP server and returns its listen URL. It binds to `127.0.0.1` by
default and returns after startup. Set `block := true` to keep the query open
until the server stops. Common options include `host`, `static_dir`,
`cors_origins`, `log_level`, `log_headers`, `log_query`, `access_log`,
`health_routes`, `worker_threads`, keep-alive and socket timeouts,
`compression`, `query_timeout_ms`, `max_response_bytes`, and
`max_pending_requests`. Request metadata is emitted through DuckDB's native
logging; header and query logging are opt-in and subject to redaction.

```sql
SELECT * FROM quackapi_serve(8000, cors_origins := 'https://app.example');
SELECT * FROM quackapi_serve(8000, block := true);
```

`quackapi_serve` does not install outbound HTTP providers. HTTPS termination
for inbound requests should be handled by the deployment's TLS endpoint.

### `quackapi_stop([port])`

Stops the selected listener, or all listeners when no port is supplied.

```sql
SELECT * FROM quackapi_stop(8000);
SELECT * FROM quackapi_stop();
```

### `quackapi_wait(port[, timeout_ms])`

Waits for a listener to become ready and returns its readiness state and URL.

```sql
SELECT * FROM quackapi_wait(8000, 5000);
```

### `quackapi_request(method, path[, body], ...)`

Executes a request through the in-process route dispatcher. This is useful for
SQL tests and local composition; it does not open a TCP connection. Optional
`headers` supplies request headers. Route response status, headers, and body
are returned as columns.

```sql
SELECT status, body
FROM quackapi_request('GET', '/health');
```

### `quackapi_routes()` and `quackapi_servers()`

Return registered route metadata and active listener metadata, respectively.
They do not expose request-ring or transport-pool diagnostics.

## Route and auth functions

### `quackapi_groups()`

Returns registered route groups, including their path prefix, inherited auth
name, and OpenAPI tags.

### `quackapi_auths()`

Lists authentication definitions without exposing API-key material or JWT
secrets.

### `quackapi_add_api_key(auth_name, key, subject[, claims])`

Adds a hashed API key to an API-key authentication definition and returns the
created key metadata. The original key is not returned by inspection
functions.

### `quackapi_apply_middleware(...)` and `quackapi_middlewares()`

`quackapi_apply_middleware` evaluates registered middleware for a request;
`quackapi_middlewares()` lists middleware metadata and registration order.
Middleware DDL and its supported phases are described in the
[DDL reference](ddl.md).

### Auth bridge functions

`quackapi_authentication`, `quackapi_authorization`, and
`quackapi_verify_auth` bridge QuackAPI auth into DuckDB's `quack_*_function`
settings. `quackapi_verify_auth` returns a boolean for each input row.

## Outbound POST

### `quackapi_post(url, body[, content_type[, headers]])`

Performs one HTTP or HTTPS POST per non-NULL URL row. The function remains
volatile and preserves the existing overloads, NULL behavior, row alignment,
and result type:

```text
status, reason, body, headers, error, reused_connection
```

Load a supported DuckDB HTTPUtil provider before calling it. `curl_httpfs` is
recommended; `httpfs` is also supported. QuackAPI does not install a provider.

```sql
LOAD curl_httpfs;
SELECT (quackapi_post(
  'https://api.example.test/items',
  '{"name":"new item"}',
  'application/json',
  MAP {'Authorization': 'Bearer …'}
)).status;
```

`quackapi_post_concurrency` bounds simultaneous requests across queries using
the same database instance. `0` selects DuckDB's configured thread count, with
a minimum of one; a positive value is an explicit worker limit. The waiting
queue is bounded and applies backpressure when full. This controls outbound
HTTP scheduling, not DuckDB SQL transaction or database-file concurrency.

For GET requests, use DuckDB's HTTP reader functions after loading the reader
extension. QuackAPI has no public fetch function or connection-pool inspection
API.
