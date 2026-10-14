# API cleanup and migration notes

QuackAPI now focuses on SQL-backed HTTP routes, route authentication, and
bounded outbound POST. Several experimental surfaces have been removed. This
guide lists the changed SQL names and the remaining boundary for each area.

## Removed SQL surfaces

| Removed surface | Migration path |
|---|---|
| `quackapi_fetch`, `quackapi_parallel_fetch`, `quackapi_http_pool()`, and `quackapi_http_util_name()` | Use DuckDB's file/HTTP readers for GET requests. For POST, use `quackapi_post`. Connection-pool inspection and the old fetch test hooks are no longer public. |
| `CREATE QUEUE`, `quackapi_enqueue/dequeue/ack/nack/renew`, `quackapi_queues()`, and `quackapi_jobs` | No built-in durable queue remains. A DuckDB table can hold application data, but it does not supply lease ownership, visibility timeout, acknowledgement, or retry semantics. The `radio` extension is a message bus; it is not a durable job queue with those guarantees. |
| `CREATE ROW ACCESS POLICY`, `CREATE MASKING POLICY`, and `quackapi_policies()` | Route authentication decides whether a caller may invoke a route; it does not filter rows or rewrite arbitrary SQL by tenant. Put tenant predicates directly in each handler or use an explicitly scoped view. |
| `CREATE STREAM`, `quackapi_streams()`, and SSE endpoints | There is no QuackAPI streaming HTTP response surface. Use ordinary bounded route responses or a separate streaming service/protocol. Loading `radio` does not add SSE support to QuackAPI. |
| GraphQL DDL, GraphQL endpoints, and GraphQL inspection functions | Expose the needed operation as a `CREATE ROUTE` handler and SQL. |
| `CREATE API FOR TABLE` and source-import/table helpers | Define the route and query explicitly with `CREATE ROUTE`; there is no generated table CRUD or Python/source importer. |
| Native PostgreSQL handler options (`pg_dsn`, `quackapi_pg_dsn`) | Connect to PostgreSQL through a separately configured DuckDB extension or client workflow. QuackAPI executes route SQL through DuckDB. |
| `FORMAT parquet` and `FORMAT arrow` response encoding | Use the supported JSON, NDJSON, or CSV HTTP response formats. Arrow Flight is a separate protocol and is not hosted by QuackAPI. DuckDB's optional `duckflight` extension serves Flight SQL separately; it is not a QuackAPI runtime dependency. |

The old server options that selected an HTTP client or native PostgreSQL path
are gone. QuackAPI does not install outbound HTTP providers automatically.

## Outbound POST

`quackapi_post` remains a volatile scalar function, including its existing
overloads and result shape:

```sql
SELECT (quackapi_post(
  'https://api.example.test/items',
  '{"name":"new item"}',
  'application/json',
  MAP {'Authorization': 'Bearer …'}
)).status;
```

The result fields are `status`, `reason`, `body`, `headers`, `error`, and
`reused_connection`. A NULL URL produces a NULL result; each non-NULL row makes
its own request. POST requests are never replayed after an ambiguous transport
failure.

Load a POST-capable DuckDB HTTPUtil provider explicitly before calling the
function. The patched `curl_httpfs` and HTTPFS-Curl providers are under
upstream review; released community binaries are not yet supported provider
builds for this contract. See [Outbound POST providers](curl_httpfs.md) for
the pinned revisions used for compatibility verification.

```sql
LOAD curl_httpfs;
LOAD quackapi;
```

Or load the patched HTTPFS-Curl `httpfs` provider instead. If no supported provider is active,
`quackapi_post` returns an actionable configuration error. It supports HTTP and
HTTPS through DuckDB's provider API; QuackAPI does not shell out to `curl` or
install either provider.

Use `quackapi_post_concurrency` to set the database-wide bound for simultaneous
requests. `0` uses DuckDB's configured thread count (minimum one); a positive
value sets the limit. Its waiting queue is bounded, so callers may experience
backpressure when all workers and queue slots are occupied. It does not change
DuckDB's SQL execution or transaction model.

## Database execution and concurrency

HTTP handlers are ordinary DuckDB SQL. The POST worker limit is not an ADBC
connection pool and does not change DuckDB transaction or file-sharing rules.
For example, a Python ADBC client can connect directly to a local DuckDB file
and execute normal SQL:

```python
import adbc_driver_duckdb.dbapi

with adbc_driver_duckdb.dbapi.connect("analytics.duckdb") as connection:
    with connection.cursor() as cursor:
        cursor.execute("SELECT count(*) FROM events")
        row = cursor.fetchone()
```

Use a separate connection per concurrent worker. DuckDB does not require one
connection or statement to be thread-safe; serialize access if sharing either
([connection](https://github.com/duckdb/duckdb/blob/v1.5.6/src/include/duckdb/common/adbc/adbc.h#L783-L784),
[statement](https://github.com/duckdb/duckdb/blob/v1.5.6/src/include/duckdb/common/adbc/adbc.h#L821-L823)).
Within one process, concurrent transactions can write when they do not conflict
on the same rows. Separate processes can open a database read-only; use a
client/server protocol for multi-process writes. The HTTP POST worker pool
does not change these limits. See [DuckDB concurrency](https://duckdb.org/docs/current/connect/concurrency)
and [DuckDB ADBC](https://duckdb.org/docs/stable/clients/adbc).

Canceling a query removes its queued requests and returns promptly. A POST
already inside the provider call cannot be rolled back; the remote server may
still complete it after the local query is canceled.
Destroying a database waits for its active provider calls to finish so worker
state remains valid. Without a caller deadline, the outbound request timeout
defaults to 600 seconds; a request already sent may complete remotely during
that wait.

## Superseded design notes

Some repository research notes describe the former queue, policy, stream,
GraphQL, import, or columnar response prototypes. They are historical snapshots,
not current API documentation. This guide and the [function reference](reference/functions.md)
describe the current surface.
