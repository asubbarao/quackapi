# Outbound HTTP and curl_httpfs

quackapi’s **inbound** server uses DuckDB’s bundled httplib. That is intentional
and unchanged by this document.

**Outbound** HTTP (anything a route handler fetches via `read_text` /
`read_json` / `read_parquet` / `read_csv` over `https://`, and C++ token-exchange
via `QuackapiHttpFetch`) goes through DuckDB’s shared `HTTPUtil` layer — never
the `curl` CLI.

## Batteries default: `curl_httpfs`

On every `quackapi_serve()`, batteries **require** the community
[curl_httpfs](https://github.com/dentiny/duckdb-curl-filesystem) extension as
the outbound HTTP client:

- libcurl **connection pooling**
- **HTTP/2**
- **async** network IO
- **100% compatible** with httpfs (`SET httpfs_client_implementation=…`)

Flow:

1. `LOAD curl_httpfs` before the listener binds
2. Verify curl_httpfs installed a curl-backed DuckDB `HTTPUtil`
3. `LOAD httpfs_timeout_retry`, which owns the timeout and retry knobs
4. Record `http_client=curl` on `/healthz`
   and `quackapi_servers()`

### Timeouts and retries belong to `httpfs_timeout_retry`

quackapi sets no outbound ceiling of its own. `HTTPUtil::InitializeParameters`
already carries `http_timeout`, `http_retries`, `http_retry_wait_ms` and
`http_retry_backoff`; the community `httpfs_timeout_retry` extension refines them
per operation (`httpfs_timeout_stat_ms`, `httpfs_retries_list`, …). It is
required at serve for the same reason curl_httpfs is: a limit only quackapi could
move is exactly the knob an operator came looking for.

```sql
INSTALL httpfs_timeout_retry FROM community;   -- one-time setup
SET http_timeout = 5000;                       -- 5s, and quackapi honours it
```

The one thing quackapi still applies is the request's own deadline: an outbound
call inside a route with `TIMEOUT 5` cannot outlive that route. It can only lower
what `http_timeout` chose, never raise it.

The serve path never installs extensions or falls back to DuckDB's stock
httplib client. If the load fails, serve fails before binding and names the
remediation: `INSTALL curl_httpfs FROM community`.

A web-framework server that fetches remote data needs a production-grade pooled
client; without it, throughput collapses under concurrency on DuckDB’s default
per-request httplib client.

### Platform coverage

Community `description.yml` for curl_httpfs excludes:

| Excluded | Available (ships binaries) |
|---|---|
| `wasm_mvp`, `wasm_eh`, `wasm_threads` | `linux_amd64`, `linux_arm64`, … |
| `windows_amd64`, `windows_amd64_mingw`, `windows_amd64_rtools` | `osx_amd64`, `osx_arm64` |

### Client selection

There is no outbound client choice. `/healthz` and `quackapi_servers()` always
report `http_client = 'curl'` after a successful serve. The compatibility input
`http_client := 'curl'` is accepted, but `http_client := 'httplib'` is rejected
and there is no `quackapi_http_client` setting.

```sql
-- Serve requires curl_httpfs and fails before binding when it is unavailable
SELECT * FROM quackapi_serve(8000);
```

When curl_httpfs cannot load:

```text
quackapi_serve: required extension curl_httpfs could not be loaded: … Install it with INSTALL curl_httpfs FROM community, then retry.
```

### Confirm the active client

```sql
-- After serve
SELECT host, port, listen_url, http_client, http_client_reason
FROM quackapi_servers();

-- Readiness JSON includes the same fields
-- GET /healthz → {"status":"ok", …, "http_client":"curl", "http_client_reason":""}

-- Active HTTPUtil name (MultiCurl / HTTPFS-Curl after curl_httpfs)
SELECT quackapi_http_util_name();
```

### Manual load (optional preloading)

Batteries still verify this on serve. Preloading is useful when another
consumer needs the same process-wide HTTPUtil:

```sql
INSTALL curl_httpfs FROM community;
LOAD curl_httpfs;
SET httpfs_client_implementation = 'curl';  -- or leave default MultiCurl
LOAD quackapi;
```

### Transparent acceleration for SQL handlers

Route handlers are ordinary SELECTs. If they call httpfs surfaces, the active
client speeds them up with no quackapi change:

```sql
CREATE ROUTE proxy GET '/proxy' AS
SELECT content FROM read_text('https://example.com/data.json');
```

See `examples/proxy_curl_httpfs.sql` if present.

### C++ outbound (OAuth/OIDC)

Use `QuackapiHttpFetch` (`src/quackapi_http_fetch.{hpp,cpp}`). It only depends
on core `HTTPUtil` headers. When curl_httpfs is loaded (including via
batteries), token-exchange POSTs use the curl client automatically. A serve
cannot start while only the built-in util is active.

## What not to do

| Anti-pattern | Why |
|---|---|
| Hard-link libcurl / curl_httpfs into quackapi | Separate extension; composition is `SetHTTPUtil` |
| `system("curl ...")` / subprocess | No secrets integration, no pool, unsafe |
| Replace the **inbound** httplib server with curl_httpfs | curl_httpfs is the **client** layer only |
| Rely on silent httplib when you need production pool/HTTP2 | Install/load curl_httpfs before serving |

## Load order

1. `LOAD quackapi` then `quackapi_serve` (serve requires curl_httpfs), **or**
2. `LOAD curl_httpfs` then `LOAD quackapi` then serve

Either order works for SQL handlers as long as the curl client is active before
the request that performs outbound I/O.
