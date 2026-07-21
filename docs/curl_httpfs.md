# Outbound HTTP and curl_httpfs

quackapi’s **inbound** server uses DuckDB’s bundled httplib. That is intentional
and unchanged by this document.

**Outbound** HTTP (anything a route handler fetches via `read_text` /
`read_json` / `read_parquet` / `read_csv` over `https://`, and C++ token-exchange
via `QuackapiHttpFetch`) goes through DuckDB’s shared `HTTPUtil` layer — never
the `curl` CLI.

## Batteries default: `curl_httpfs`

On every `quackapi_serve()`, batteries **prefer** the community
[curl_httpfs](https://github.com/dentiny/duckdb-curl-filesystem) extension as
the outbound HTTP client:

- libcurl **connection pooling**
- **HTTP/2**
- **async** network IO
- **100% compatible** with httpfs (`SET httpfs_client_implementation=…`)

Flow (same pattern as auto-LOAD companions):

1. Best-effort `LOAD` / `INSTALL` **httpfs**
2. `INSTALL curl_httpfs FROM community` + `LOAD curl_httpfs` (if not already loaded)
3. `SET httpfs_client_implementation = 'curl'`
4. Log `quackapi.http_client=curl` and record `http_client=curl` on `/healthz`
   and `quackapi_servers()`

A web-framework server that fetches remote data needs a production-grade pooled
client; without it, throughput collapses under concurrency on DuckDB’s default
per-request httplib client.

### Platform coverage + graceful fallback

Community `description.yml` for curl_httpfs excludes:

| Excluded | Available (ships binaries) |
|---|---|
| `wasm_mvp`, `wasm_eh`, `wasm_threads` | `linux_amd64`, `linux_arm64`, … |
| `windows_amd64`, `windows_amd64_mingw`, `windows_amd64_rtools` | `osx_amd64`, `osx_arm64` |

If `INSTALL`/`LOAD` fails for any reason (unsupported platform, offline
catalog, older DuckDB), **serve does not fail**. Batteries stay on the stock
httplib client and log:

```text
quackapi.http_client=httplib reason=curl_httpfs_unavailable
```

`/healthz` then reports `"http_client":"httplib"`.

### Override knob

| Surface | Values | Default |
|---|---|---|
| `quackapi_serve(…, http_client := '…')` | `auto` \| `curl` \| `httplib` | `auto` |
| `SET quackapi_http_client = '…'` | same | `auto` |

- **`auto` / `curl`** — prefer curl_httpfs; fall back to httplib if unavailable
- **`httplib`** — skip curl_httpfs install; force stock client  
  logs `quackapi.http_client=httplib reason=operator_forced`

Named param wins over the SET.

```sql
-- Default (prefer curl_httpfs)
SELECT * FROM quackapi_serve(8000);

-- Force stock httplib client
SELECT * FROM quackapi_serve(8000, http_client := 'httplib');

SET quackapi_http_client = 'curl';
SELECT * FROM quackapi_serve(8001);
```

### Confirm the active client

```sql
-- After serve
SELECT * FROM quackapi_servers();
-- host | port | listen_url | http_client

-- Readiness JSON includes the same field
-- GET /healthz → {"status":"ok", …, "http_client":"curl"}

-- Active HTTPUtil name (MultiCurl / HTTPFS-Curl after curl_httpfs, Built-In otherwise)
SELECT quackapi_http_util_name();
```

### Manual load (optional)

Batteries already do this on serve. Manual load is still fine for non-serve
sessions:

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
batteries), token-exchange POSTs use the curl client automatically. When only
the built-in util is active, POST fails fast with a message to load curl_httpfs.

## What not to do

| Anti-pattern | Why |
|---|---|
| Hard-link libcurl / curl_httpfs into quackapi | Separate extension; composition is `SetHTTPUtil` |
| `system("curl ...")` / subprocess | No secrets integration, no pool, unsafe |
| Replace the **inbound** httplib server with curl_httpfs | curl_httpfs is the **client** layer only |
| Fail serve when curl_httpfs is missing | Batteries must boot on every platform in `excluded_platforms` for quackapi |

## Load order

1. `LOAD quackapi` then `quackapi_serve` (batteries prefer curl_httpfs for you), **or**
2. `LOAD curl_httpfs` then `LOAD quackapi` then serve

Either order works for SQL handlers as long as the curl client is active before
the request that performs outbound I/O.
