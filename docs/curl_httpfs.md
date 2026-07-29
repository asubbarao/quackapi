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

### Platform coverage

Community `description.yml` for curl_httpfs excludes:

| Excluded | Available (ships binaries) |
|---|---|
| `wasm_mvp`, `wasm_eh`, `wasm_threads` | `linux_amd64`, `linux_arm64`, … |
| `windows_amd64`, `windows_amd64_mingw`, `windows_amd64_rtools` | `osx_amd64`, `osx_arm64` |

### Preference semantics (`http_client`)

| Surface | Values | Default |
|---|---|---|
| `quackapi_serve(…, http_client := '…')` | `auto` \| `curl` \| `httplib` | `auto` |
| `SET quackapi_http_client = '…'` | same | `auto` |

Named param wins over the SET.

| Value | Behavior |
|---|---|
| **`auto`** | Prefer curl_httpfs. If INSTALL/LOAD fails, **fall back to httplib** and report **loudly**: stderr line, `/healthz` `http_client` + `http_client_reason`, `quackapi_servers()`. |
| **`curl`** | **Require** curl_httpfs. If INSTALL/LOAD fails, **serve fails** (no silent fallback). Production guarantee. |
| **`httplib`** | Skip curl_httpfs install; force stock client. Logs `reason=operator_forced`. |

```sql
-- Default (prefer curl_httpfs; loud fallback if unavailable)
SELECT * FROM quackapi_serve(8000);

-- Production: require curl_httpfs or refuse to serve
SELECT * FROM quackapi_serve(8000, http_client := 'curl');

-- Explicit stock client (dev / unsupported platform)
SELECT * FROM quackapi_serve(8000, http_client := 'httplib');

SET quackapi_http_client = 'curl';
SELECT * FROM quackapi_serve(8001);
```

### auto fallback (loud, never silent)

When `http_client` is `auto` and curl_httpfs cannot install/load:

```text
quackapi.http_client=httplib reason=curl_httpfs_unavailable WARN=auto_fallback …
```

`/healthz` then reports:

```json
{"status":"ok", …, "http_client":"httplib", "http_client_reason":"curl_httpfs_unavailable"}
```

`quackapi_servers()` has the same `http_client` / `http_client_reason` columns.

### Production recommendation

On **linux** and **osx** (platforms where community curl_httpfs ships), production
should either:

1. **`http_client := 'curl'`** (or `SET quackapi_http_client = 'curl'`) so serve
   fails closed if the pooled client is missing, **or**
2. Keep `auto` but **alert on** `/healthz` when
   `http_client_reason == "curl_httpfs_unavailable"`.

Do not ship concurrent outbound https under silent httplib fallback.

### Confirm the active client

```sql
-- After serve
SELECT host, port, listen_url, http_client, http_client_reason
FROM quackapi_servers();

-- Readiness JSON includes the same fields
-- GET /healthz → {"status":"ok", …, "http_client":"curl", "http_client_reason":""}

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
| Rely on silent httplib when you need production pool/HTTP2 | Use `http_client := 'curl'` or alert on `http_client_reason` |

## Load order

1. `LOAD quackapi` then `quackapi_serve` (batteries prefer curl_httpfs for you), **or**
2. `LOAD curl_httpfs` then `LOAD quackapi` then serve

Either order works for SQL handlers as long as the curl client is active before
the request that performs outbound I/O.
