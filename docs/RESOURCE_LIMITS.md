# Request budgets and admission limits

QuackAPI separates SQL execution budgets from socket I/O timeouts. `TIMEOUT` on
a route continues to configure socket reads and writes. It does not replace the
query budget.

```sql
SELECT * FROM quackapi_serve(
  '127.0.0.1', 8000,
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
An oversized ordinary response returns 507. Row serialization stops when its
running size estimate exceeds the cap, and the completed uncompressed body is
checked before compression. The response cap does not replace DuckDB's
`memory_limit`: query operators and materialized results can allocate memory
before serialization.

The optional native PostgreSQL path uses nonblocking connection/query polling
under the same deadline and assembles capped JSON as rows arrive. It retains a
bounded session statement timeout without adding a `SET` round trip to every
ordinary request. PostgreSQL execution or transport failures return a sanitized
502 (504 for timeout). A failed PostgreSQL command is never replayed against
DuckDB; only a handler identified as inapplicable to native execution can fall
back. A connection failure after a write was sent can leave the write outcome
unknown, so applications should use idempotency keys when retrying writes.

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
