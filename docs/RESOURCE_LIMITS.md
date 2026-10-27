# Request budgets and admission limits

QuackAPI separates SQL execution budgets from socket I/O timeouts. `TIMEOUT` on
a route continues to configure socket reads and writes. It does not replace the
query budget.

```sql
SELECT * FROM quackapi_serve(
  8000, host := '127.0.0.1',
  query_timeout_ms := 30000,
  max_response_bytes := 16777216,
  max_pending_requests := 256
);

SELECT status, body FROM quackapi_request(
  'GET', '/report', query_timeout_ms := 1000, max_response_bytes := 1048576
);
```

The defaults are a 30-second execution budget, a 16 MiB response cap, and 256
pending HTTP worker tasks (accepted connections). Configure the defaults with
`quackapi_query_timeout_ms`, `quackapi_max_response_bytes`, and
`quackapi_max_pending_requests`, or pass the named server options. In-process
requests accept the execution and response options too. All limits must be
positive; their maximum values are one day, 1 GiB, and 100,000 pending requests.

A shared watchdog interrupts an active DuckDB connection when its budget
expires. Middleware and GraphQL inherit the request budget; outbound HTTP
workers inherit the current deadline. An expired ordinary request returns 504.
An oversized ordinary response returns 507. JSON rows are encoded once while
fetching DuckDB result chunks, with the exact JSON byte count checked before
each row is appended. Arrow responses project the native chunk vectors into
DuckDB's Arrow converter and check the encoded size after each record batch.
Both formats retain the completed HTTP body in memory; Arrow IPC stream format
does not imply progressive network delivery. The completed uncompressed body is
checked before compression. The response cap does not replace DuckDB's
`memory_limit`: query operators and materialized results can allocate memory
before serialization.

Parquet output consumes native result chunks without executing the handler
again. It appends those chunks to a temporary staging table and uses DuckDB
2.0's submitted native `COPY` query to write a temporary file. The completed file
size is checked before allocating the HTTP body; both temporary objects are
removed on success or failure. The staging data and file can grow before this
final encoded-size check, so DuckDB memory and temporary-storage limits still
matter.

`quackapi_selfdispatch` collects native `rows` and `columns` before JSON
serialization. The serialized native result is checked incrementally against
the response cap, including schema and row-list overhead. Typed
`quackapi_post(..., response_format := 'arrow'|'parquet')` applies the same check
while decoding. The incoming HTTP body is buffered before its size check; the
cap is not a transport-level download or heap-allocation limit. A decode failure
retains the original HTTP status and bytes, records `error`, and leaves typed
fields NULL. The enclosing request's deadline and response cap take precedence
over session defaults.

PostgreSQL access uses DuckDB's `postgres` extension and its connection and
query controls. The former quackapi-specific `pg_dsn` execution path is removed.

The HTTP worker pool rejects enqueue attempts when its bounded connection queue is
full. Rejected connections are closed; the admission mechanism does not promise
an HTTP 503 response. Active workers remain bounded by `worker_threads`.

SSE applies a finite execution budget to each query/fetch operation and a
cumulative output cap. Once headers have been sent, a limit closes the stream;
it cannot change its HTTP status. The initial result is retained for streaming,
so starting a stream executes its query once. Long-lived idle streams are not
terminated merely because the ordinary request budget elapsed.

Cancellation is cooperative. DuckDB operators must observe interruption, and
extensions must honor their I/O timeouts. This is not a process-level CPU or
memory sandbox for arbitrary native extensions.

## Rate-limit identity

Buckets belong to a DuckDB instance and are cleared when their route is replaced
or dropped. Fixed windows use a monotonic clock. A database holds at most
100,000 active buckets; when that bound is reached, new identities are denied
until entries expire.

Authenticated `BY token` and `BY key` limits use the verified subject when
available, otherwise a credential digest. Equivalent accepted Bearer spelling
and API-key header encodings share a quota. Public routes and invalid credentials
use the peer IP, so an arbitrary caller-supplied token cannot create a fresh
quota. Raw credentials are not retained in limiter keys. Separate processes
still have separate quotas; this is not a distributed rate limiter.

See `test/sql/quackapi_resource_limits.test` for timeout, post-cancellation
recovery, response-size, configuration, and credential-normalization regressions.

Outbound keep-alive pooling retains at most 256 idle clients across at most
256 host entries, with a five-minute idle expiry checked on pool activity.
These are idle-pool limits; they do not cap all concurrently active outbound
requests or the size of an upstream response.

## Upload limits and disconnects

The server `max_request_bytes` option (or `quackapi_max_request_bytes` setting)
is a hard ceiling, default 8 MiB and maximum 1 GiB. A route may opt into a larger
or smaller limit with `WITH (max_request_bytes := ...)`. Its effective limit is
the smaller of the route limit and server ceiling. Routes with no override keep
8 MiB even when the server ceiling is raised. The header/body reader enforces
this for Content-Length and chunked transfers; supported request decompression
is also bounded. Unsupported request content encodings return 415.

`cancel_on_disconnect := true` on `quackapi_serve` enables ordinary-route
cancellation on downstream socket EOF. The default is false to preserve clients
that intentionally half-close writes before reading their response. A shared
watchdog interrupts the query and shuts down an active pooled plain-HTTP POST
socket; it does not allocate a thread for each request. HTTPS backends have no
portable abort method in DuckDB's public HTTPClient interface and remain bounded
by their per-call timeout. A closed upstream connection does not guarantee that
a remote generation job stops; use the upstream job-cancellation API for that.

The community `httpfs_timeout_retry` extension controls filesystem operations
(open/read/write, listing and metadata). It does not wrap quackapi's direct
pooled POST calls. Use `timeout_ms` on `quackapi_post` for those, with an outer
query budget large enough for generation. Its explicit value overrides the
session's outbound default but never extends the outer budget.
