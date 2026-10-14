# Outbound HTTP providers

`quackapi_post` sends outbound HTTP and HTTPS requests through DuckDB's
`HTTPUtil` provider API. Load a supported provider explicitly before the first
call. QuackAPI does not install or select a provider automatically.

`curl_httpfs` is recommended. `httpfs` is also supported. The `curl_httpfs`
provider can provide connection reuse and HTTP/2 according to its own build
and configuration; the exact behavior depends on the provider version and
platform.

```sql
LOAD curl_httpfs;
LOAD quackapi;

SELECT (quackapi_post(
  'https://api.example.test/items',
  '{"name":"new item"}',
  'application/json',
  MAP {'Authorization': 'Bearer …'}
)).status;
```

To use `httpfs`, load it instead:

```sql
LOAD httpfs;
LOAD quackapi;
```

If neither supported HTTPUtil provider is loaded, `quackapi_post` returns a
configuration error for each non-NULL input row. Each input row retains its
position and produces one aligned result. Requests are not retried after
transport errors.

Sub-second request deadlines use DuckDB's `HTTPParams::timeout_usec` field.
The `curl_httpfs` MultiCurl implementation still needs the provider fix in
[duckdb-curl-filesystem PR #75](https://github.com/dentiny/duckdb-curl-filesystem/pull/75)
to honor that fractional remainder. The `httpfs` HTTPFS-Curl path also has a
separate compatibility change pending. Treat sub-second deadline enforcement
as pending until the corresponding provider fix is merged and present in the
loaded provider.

## GET requests

For GET requests inside DuckDB SQL, use DuckDB's reader functions such as
`read_text`, `read_json`, and `read_csv` after loading the extension that
provides the reader. QuackAPI has no public `quackapi_fetch` function.

```sql
LOAD httpfs;

SELECT content
FROM read_text('https://example.com/data.txt');
```

QuackAPI's inbound server is separate from outbound provider selection. It
continues to use DuckDB's bundled HTTP server implementation. The provider
does not replace the server and does not add TLS termination for inbound
connections.

See the [POST function reference](reference/functions.md#outbound-post) and
[migration guide](MIGRATION.md) for the retained SQL contract and removed
transport APIs.
