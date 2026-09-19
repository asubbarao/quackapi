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

The defaults are a 30-second execution budget and a 16 MiB response cap.

The HTTP budget is one dial. `worker_threads` (default 32, maximum 4096) is how
many requests run at once; `max_pending_requests` is derived from it as eight
accepted connections per worker — 256 at the default — unless the operator names
it or sets `quackapi_max_pending_requests`, which pins the queue independently.
Raising `worker_threads` therefore raises the queue with it, instead of leaving
a sweep to cross a queue size it never chose. Neither is the DuckDB `threads`
setting: that is the query budget, a different resource, and `quackapi_serve()`
does not touch it.

Configure the other defaults with `quackapi_query_timeout_ms` and
`quackapi_max_response_bytes`, or pass the named server options. In-process
requests accept the execution and response options too. All limits must be
positive; their maximum values are one day, 1 GiB, and 100,000 pending
requests.

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

Past `worker_threads` active plus `max_pending_requests` queued, a connection is
shed with `503 Service Unavailable`, `Retry-After: 1`, `X-Quackapi-Budget:
pending` and a body naming both numbers. It is answered on the accept thread and
never reaches a route or a DuckDB connection. The `::listen()` backlog is sized
to at least `SOMAXCONN` so the kernel does not refuse a burst before quackapi
can answer it; the OS still clamps that to `kern.ipc.somaxconn` /
`net.core.somaxconn`.

`quackapi_servers()` reports the budget and what it has done: `worker_threads`,
`max_pending_requests`, `workers_peak` (deepest concurrent in-flight requests),
`shed_requests`, and `binding_budget` — `none` while the worker budget has never
filled, `workers` once it has, `pending` once connections have been shed. The
first shed also writes one line to stderr at WARN.

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
