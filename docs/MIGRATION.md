# Upgrading to 0.2.0: the simplified API

The 0.2.0 release candidate targets DuckDB **2.0.0**. Source documentation describes this
checkout; `INSTALL quackapi FROM community` follows the separate commit pinned
in the community registry. A green repository build does not publish that build
to the community CDN.

| Previous surface | Replacement |
|------------------|-------------|
| `quackapi_fetch`, `quackapi_parallel_fetch` | Native `read_text`, `read_blob`, CSV/JSON/Parquet readers for remote files; community `http_request` for arbitrary GET status and headers |
| Eight framework-specific route/model functions | `quack_from_routes(framework, path)` and `quack_from_models(framework, path)`; frameworks: `fastapi`, `rails`, `express`, `gin` |
| Public importer SQL inspection helpers | Maintained extraction SQL in `src/sql/`; the two import functions return metadata rows |
| `quackapi_serve(..., pg_dsn := ...)`, `quackapi_pg_dsn` | `INSTALL postgres; LOAD postgres; ATTACH '…' AS pg (TYPE postgres)` and ordinary SQL against `pg`; nonempty legacy DSNs fail explicitly |
| Quack auth adapters / automatic REST auth wiring | Configure native DuckDB Quack hooks separately; REST `CREATE AUTH` and `REQUIRE` remain available |
| Standalone HTTP-client / last-write-timeout diagnostics | `quackapi_servers()`, structured request logs, and configured deadlines |

## POST receipts and deadlines

`quackapi_post` remains available in scalar and table forms. Its receipt is
`STRUCT(status, reason, body, headers, error, reused_connection, body_blob)`.
Use `body_blob` for exact bytes; `body` is NULL for invalid UTF-8. Callers that
unpack the entire STRUCT must account for the added field.

```sql
SELECT status, body_blob
FROM quackapi_post('https://api.example.com/jobs', '{"run":true}',
                  content_type := 'application/json', timeout_ms := 120000);
```

`timeout_ms` must be positive. It overrides `quackapi_http_timeout_ms` for that
call, but the enclosing query/route deadline still wins. The outbound pool is
bounded to 64 idle clients per origin, 256 idle clients overall, and 256 origin
entries. Overflow clients are not retained for reuse.

`quackapi_serve(..., cancel_on_disconnect := true)` opts into interrupting route
SQL and aborting blocked plain-HTTP POSTs when the caller disconnects. HTTPS
cancellation depends on the HTTPUtil backend and remains bounded by its timeout.
Remote jobs require the remote service's cancellation API. SSE polls SQL state;
outbound POST waits for a complete response and does not relay token streams.

## Uploads, policies, validation, and queues

- Uploads default to **8 MiB**. The server's `max_request_bytes` is a hard ceiling
  (maximum **1 GiB**). A route's `WITH (max_request_bytes = …)` opts into a larger
  payload within that ceiling; raising only the server option leaves ordinary
  routes at 8 MiB. Both chunked and Content-Length requests are enforced.
- Policy rewriting follows CTE scope, including same-name physical tables and
  ordered CTE definitions. Unsupported policy-sensitive macros fail closed.
  Exec routes and `quackapi_exec` fail closed while policies are bound.
- Explicit parameter sources are respected. Length constraints count Unicode
  code points. `BODY TYPE` binds typed values and OpenAPI uses the same types.
- FastAPI model extraction treats `Field()`, metadata-only `Field(...)`, and
  `default=...` as required. Real defaults/default factories are distinguished
  structurally; extraction does not execute Python factories or validators.
- Queue mutations execute at runtime, including under prepared statements;
  planning or explaining a statement does not enqueue/acknowledge jobs.
  Workers must keep the `delivery_generation` fence for ack/nack/renew.
- Table list routes validate pagination and order by their key. API publication
  is atomic when route creation fails.

See the [function reference](reference/functions.md),
[upload guide](guide/request-bodies.md), [policy guide](guide/policies.md), and
[framework import guide](from-fastapi.md) for application examples.
