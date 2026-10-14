# Request budgets and concurrency

QuackAPI applies limits at the HTTP boundary while route handlers remain
ordinary DuckDB SQL.

```sql
SELECT * FROM quackapi_serve(
  8000, host := '127.0.0.1',
  query_timeout_ms := 30000,
  max_response_bytes := 16777216,
  max_pending_requests := 256
);

SELECT status, body FROM quackapi_request(
  'GET', '/report', query_timeout_ms := 1000,
  max_response_bytes := 1048576
);
```

The defaults are a 30-second route-query budget, a 16 MiB uncompressed
response cap, and 256 pending accepted connections. Configure defaults with
`quackapi_query_timeout_ms`, `quackapi_max_response_bytes`, and
`quackapi_max_pending_requests`, or override them on `quackapi_serve` and
`quackapi_request`. The named limits must be positive. A route's `TIMEOUT` or
`WITH (timeout_sec := …)` extends that request's socket read/write deadline; it
does not change the SQL query budget.

If the bounded HTTP connection queue is full, a new accepted connection is
closed. The admission mechanism does not promise an HTTP 503 response. The
response-size cap applies to the uncompressed response body; DuckDB query
operators and materialized results may allocate memory before serialization,
so this is not a substitute for DuckDB's `memory_limit`.

## Outbound POST concurrency

`quackapi_post_concurrency` is the database-wide limit for active outbound POST
requests. A value of `0` follows DuckDB's configured thread count with a
minimum of one; a positive value sets the concurrency explicitly. The waiting
queue is bounded to twice the effective concurrency. When both workers and
queue slots are full, caller-side backpressure applies. This setting bounds the
POST transport only; it does not schedule route SQL or change transaction
semantics.

## Ordinary SQL concurrency

Handlers execute ordinary SQL against DuckDB. Concurrent HTTP requests do not
create an ADBC connection pool, per-request database server, or multiple-writer
mode. The usual DuckDB transaction and database-file locking rules remain in
force. Treat this as an embedded analytical database with an HTTP interface,
not as a high-write OLTP service.

See [DuckDB concurrency guidance](https://duckdb.org/docs/stable/connect/concurrency)
for the database's ordinary SQL concurrency behavior. Arrow Flight and ADBC
are separate client/protocol surfaces; neither changes QuackAPI route SQL or
adds a Flight server here.
