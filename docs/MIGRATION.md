# API cleanup and migration notes

QuackAPI now focuses on SQL-backed HTTP routes, route authentication, and
bounded outbound POST. Several experimental surfaces have been removed. This
guide lists the changed SQL names and the remaining boundary for each area.

## Removed SQL surfaces

| Removed surface | Migration path |
|---|---|
| `quackapi_fetch`, `quackapi_parallel_fetch`, `quackapi_http_pool()`, and `quackapi_http_util_name()` | Use DuckDB's file/HTTP readers for GET requests. For POST, use `quackapi_post`. Connection-pool inspection and the old fetch test hooks are no longer public. |
| `CREATE QUEUE`, `quackapi_enqueue/dequeue/ack/nack/renew`, `quackapi_queues()`, and `quackapi_jobs` | No built-in durable queue remains. A DuckDB table can hold application data, but it does not supply lease ownership, visibility timeout, acknowledgement, or retry semantics. The `radio` extension is a message bus; it is not a durable job queue with those guarantees. |
| `CREATE ROW ACCESS POLICY`, `CREATE MASKING POLICY`, and `quackapi_policies()` | Route authentication is still available, but it does not filter rows or rewrite arbitrary SQL by tenant. Put tenant predicates directly in each handler or in an explicitly scoped view, and use DuckDB privileges for database access. |
| `CREATE STREAM`, `quackapi_streams()`, and SSE endpoints | There is no QuackAPI streaming HTTP response surface. Use ordinary bounded route responses or a separate streaming service/protocol. Loading `radio` does not add SSE support to QuackAPI. |
| GraphQL DDL, GraphQL endpoints, and GraphQL inspection functions | Expose the needed operation as a `CREATE ROUTE` handler and SQL. |
| `CREATE API FOR TABLE` and source-import/table helpers | Define the route and query explicitly with `CREATE ROUTE`; there is no generated table CRUD or Python/source importer. |
| Native PostgreSQL handler options (`pg_dsn`, `quackapi_pg_dsn`) | Connect to PostgreSQL through a separately configured DuckDB extension or client workflow. QuackAPI executes route SQL through DuckDB. |
| `FORMAT parquet` and `FORMAT arrow` response encoding | Use the supported JSON, NDJSON, or CSV HTTP response formats. Arrow Flight is a separate protocol and is not hosted by QuackAPI. DuckDB Flight clients such as ADBC/Airport remain separate tools; neither is a QuackAPI runtime dependency. |

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
function. `curl_httpfs` is recommended; `httpfs` is also supported. For example:

```sql
LOAD curl_httpfs;
LOAD quackapi;
```

Or load the `httpfs` provider instead. If no supported provider is active,
`quackapi_post` returns an actionable configuration error. It supports HTTP and
HTTPS through DuckDB's provider API; QuackAPI does not shell out to `curl` or
install either provider.

Use `quackapi_post_concurrency` to set the database-wide bound for simultaneous
requests. `0` uses DuckDB's configured thread count (minimum one); a positive
value sets the limit. Its waiting queue is bounded, so callers may experience
backpressure when all workers and queue slots are occupied. It does not change
DuckDB's SQL execution or transaction model.

## Database execution and concurrency

HTTP handlers are ordinary DuckDB SQL. The number of HTTP worker threads is not
an ADBC connection pool and does not create multiple-writer semantics for a
DuckDB database file. Existing DuckDB transaction, locking, and file-sharing
constraints still apply. Keep writes appropriately scoped and use DuckDB's
documented concurrency model; QuackAPI is not a high-write OLTP application
server.

## Superseded design notes

Some repository research notes describe the former queue, policy, stream,
GraphQL, import, or columnar response prototypes. They are historical snapshots,
not current API documentation. This guide and the [function reference](reference/functions.md)
describe the current surface.
