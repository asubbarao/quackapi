# Outbound HTTP and curl_httpfs

quackapi’s **inbound** server uses DuckDB’s bundled httplib. That is intentional
and unchanged.

**Outbound** HTTP (anything a route handler fetches, and future OAuth/OIDC token
exchange) must go through DuckDB’s shared `HTTPUtil` layer — never the `curl`
CLI.

## Recommended companion: `curl_httpfs`

[curl_httpfs](https://github.com/dentiny/duckdb-curl-filesystem) is a community
extension that replaces the active `HTTPUtil` with a libcurl implementation
(default name `MultiCurl`: HTTP/2, connection pooling, async IO). It is 100%
compatible with httpfs and will load httpfs semantics for you if needed.

```sql
INSTALL curl_httpfs FROM community;
LOAD curl_httpfs;   -- active util becomes MultiCurl
LOAD quackapi;
```

### Transparent acceleration for SQL handlers

Route handlers are ordinary SELECTs. If they call httpfs surfaces, curl_httpfs
speeds them up with no quackapi change:

```sql
CREATE ROUTE proxy GET '/proxy/:url' AS
SELECT content FROM read_text($url);
```

See `examples/proxy_curl_httpfs.sql`.

### Confirm the client

```sql
-- Provided by curl_httpfs:
SELECT curl_httpfs_http_util_name();
-- MultiCurl

-- Provided by quackapi (after this integration):
SELECT quackapi_http_util_name();
-- MultiCurl   (same GetName() under the hood)

SET curl_httpfs_client_implementation = 'httplib';  -- fall back
SET curl_httpfs_client_implementation = 'multi_curl';
SET curl_httpfs_enable_verbose_logging = true;       -- libcurl -v to stderr
```

### C++ outbound (OAuth/OIDC)

Use `QuackapiHttpFetch` (`src/quackapi_http_fetch.{hpp,cpp}`). It only depends
on core `HTTPUtil` headers. When curl_httpfs is loaded, token-exchange POSTs
use MultiCurl automatically. When only the built-in util is active, POST fails
fast with a message to `LOAD curl_httpfs`.

## What not to do

| Anti-pattern | Why |
|---|---|
| Hard-link libcurl / curl_httpfs into quackapi | Separate extension; composition is `SetHTTPUtil` |
| `system("curl ...")` / subprocess | No secrets integration, no pool, unsafe |
| Assume httpfs is already LOADed | curl_httpfs embeds/loads it; don’t dual-link |

## Load order

1. `LOAD curl_httpfs` (or `LOAD httpfs` if you only need the stock curl client)
2. `LOAD quackapi`
3. `CREATE ROUTE` / `quackapi_serve`

Either load order works for SQL handlers as long as curl_httpfs is loaded
before the request that performs outbound I/O.
