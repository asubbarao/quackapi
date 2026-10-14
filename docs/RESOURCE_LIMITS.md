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

The worker pool grows when needed and retains its workers while the database is
open. Lowering the concurrency setting limits future active work but does not
shrink workers that have already started; their provider client state remains
owned by those workers until database shutdown.

Query cancellation promptly removes queued POSTs. A provider call already in
progress may still complete at the remote server. Database shutdown waits for
active provider calls to return so their worker state stays valid; without a
caller deadline, the outbound request timeout defaults to 600 seconds.

## Ordinary SQL concurrency

Handlers execute ordinary SQL against DuckDB. Concurrent HTTP requests do not
create an ADBC connection pool, per-request database server, or multiple-writer
mode. DuckDB permits concurrent writes from threads in one process when
transactions do not conflict on the same rows. Separate processes may open a
database read-only; multi-process writes require a client/server protocol.
QuackAPI's POST worker setting does not change those rules.

ADBC can connect directly to DuckDB for ordinary SQL, for example:

```python
import adbc_driver_duckdb.dbapi

with adbc_driver_duckdb.dbapi.connect("analytics.duckdb") as connection:
    with connection.cursor() as cursor:
        cursor.execute("SELECT count(*) FROM events")
        print(cursor.fetchone())
```

Use one ADBC connection per concurrent worker. If a connection or statement is
shared, serialize access; DuckDB's ADBC implementation does not require either
to be thread-safe ([connection](https://github.com/duckdb/duckdb/blob/v1.5.6/src/include/duckdb/common/adbc/adbc.h#L783-L784),
[statement](https://github.com/duckdb/duckdb/blob/v1.5.6/src/include/duckdb/common/adbc/adbc.h#L821-L823)).
The optional [`duckflight` extension](https://duckdb.org/community_extensions/extensions/duckflight)
serves PostgreSQL and Arrow Flight SQL endpoints as a separate protocol; it is
not required by QuackAPI. See [DuckDB concurrency](https://duckdb.org/docs/current/connect/concurrency)
and [DuckDB ADBC](https://duckdb.org/docs/stable/clients/adbc).
