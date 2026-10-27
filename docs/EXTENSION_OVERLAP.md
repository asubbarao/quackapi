# DuckDB extension overlap

Use DuckDB's `postgres` extension and an attached PostgreSQL catalog for
PostgreSQL access. The separate libpq request path has been removed. Existing
nonempty `pg_dsn` settings fail with a migration message so they cannot silently
switch databases.

For remote files, DuckDB's `httpfs` and readers such as `read_json`, `read_csv`,
and `read_parquet` can load and parse data directly. For general HTTP methods,
the community `http_request` extension provides `http_get`, `http_post`, and
other methods with concurrent requests within a chunk. It is distinct from
the community `http_client` extension; the two expose overlapping SQL names.
`httpfs` readers are suitable for remote files, but do not expose a general
HTTP response with status and headers. `http_request` can provide those for a
GET, returning the body as a BLOB. The removed `quackapi_fetch` returned a VARCHAR body and
additional reason, error and connection-reuse fields. Its socket-stall test seam has also been removed from the production client. Migrating calls that only read
data is straightforward; migrating the complete API changes these contracts.

For POST, `http_request` supports a body through its table function, but its
scalar `http_post` overloads accept URL, headers and query parameters, not a
raw body. A lateral table call with a per-row body is rejected by DuckDB's
binder. Consequently it cannot currently replace `quackapi_post`'s concurrent
per-row POST bodies. In a local HTTP/1.1 test, two separate table POST queries
used different TCP connections, while two `quackapi_post` queries reused one.
DuckDB's own HTTP pool does not imply that this community extension uses it.

In the tested `http_request` build, the default cache suppressed the second of
two identical scalar POSTs in separate queries; disabling `http_request_cache`
sent both. The upstream source currently implements a process-global one-second
cache for scalar calls, despite the setting description saying “within query”.
The upstream source also disables TLS certificate verification; the released
binary's TLS behavior has not been confirmed. See the upstream
[implementation](https://github.com/midwork-finds-jobs/duckdb_http_request/blob/main/src/http_request_extension.cpp).
These observations mean that
switching to it requires attention to side effects, credentials and security.
The `quackapi_post` function remains while these gaps are unresolved.
`quackapi_fetch` has been removed; use native readers for files and
`http_request` for HTTP response metadata; `quackapi_selfdispatch` remains an independent in-process
SQL execution function.

A local DuckDB 2.0.0 probe with eight distinct URLs, a 200 ms server delay,
four DuckDB threads, eight async threads, and request caching disabled observed
eight concurrent requests for both `http_request` and `quackapi_fetch`, each
finishing in roughly 210 ms. `read_json` reached four concurrent requests and
made 16 GETs for the eight URLs in this setup. This is one synthetic GET test,
not a comparison of POST, remote TLS, error handling, or larger workloads.
DuckDB 2.0's asynchronous readers include Parquet and eligible CSV files;
JSON reads remain synchronous per worker, which still permits multiple workers
to overlap.

The server's JSON and CSV response serialization is another overlap to review
against DuckDB's native writers, while preserving response shapes and HTTP
metadata. Parquet serialization already delegates to DuckDB. DuckDB's `quack`
extension serves a different protocol from quackapi's HTTP routes.

References: [core extensions](https://duckdb.org/docs/2.0/core_extensions/overview),
[asynchronous I/O](https://duckdb.org/docs/2.0/guides/performance/how_to_tune_workloads),
[http_request](https://duckdb.org/community_extensions/extensions/http_request),
and [http_client](https://duckdb.org/community_extensions/extensions/http_client).

## Public API cleanup

The eight framework-specific importer names are replaced by
`quack_from_routes(framework, path)` and `quack_from_models(framework, path)`.
The SQL text/path inspection helpers are no longer registered; their drift
checks run in `test/test_from_x_sql_embed.py`.

`quackapi_parallel_fetch` and the custom GET implementation are removed.
Use native readers for files, or community `http_get` for status/headers.
Socket-write deadline tests use an external client rather than a public
last-write-timeout function. `quackapi_http_util_name` is removed; the companion
`curl_httpfs` extension exposes its own backend diagnostic, and quackapi's
server diagnostics retain backend/fallback information.

Quackapi no longer registers RPC authentication/authorization adapters or
changes core Quack's callback settings when serving REST. Configure native
Quack credentials and hooks directly; REST `CREATE AUTH` schemes do not
implicitly grant RPC access. REST auth verification and the queue's fenced
`ack`/`renew` operations remain unchanged.
