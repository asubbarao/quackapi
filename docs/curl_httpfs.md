# Outbound HTTP providers

`quackapi_post` sends outbound HTTP and HTTPS requests through DuckDB's
`HTTPUtil` provider API. Load a supported provider explicitly before the first
call. QuackAPI does not install or select a provider automatically.

The `curl_httpfs` and HTTPFS-Curl provider changes needed for this contract are
under upstream review. Do not treat released community binaries as supported
providers yet. Compatibility verification uses the pinned provider builds
below. `curl_httpfs` may provide connection reuse and HTTP/2 according to its
build and configuration; exact behavior depends on provider version and
platform.

The current integration workflow builds both providers against DuckDB
`069cc9f9b5be802405797faecc284961b07c70ef` and runs the same real-HTTP POST
contract tests against each artifact. The `curl_httpfs` source pin is
[`a48f83c`](https://github.com/asubbarao-ifr/duckdb-curl-filesystem/tree/a48f83c6198a0252672f6f696e446e1525745a8a),
which includes provider base `f253956`, timeout fix `e10d372`, POST
Content-Type fix `185b967`, and MultiCurl handle reuse fix `c51455d`. The
HTTPFS-Curl source pin is
[`748f5ac`](https://github.com/asubbarao-ifr/duckdb-httpfs/tree/748f5ac8dddee531384d9901387d43b0f6a2db43),
based on `b26737e`. These pins are test inputs; they do not indicate released
provider support. The integration checks exercise each provider independently
and replace HTTPFS-Curl with MultiCurl while an HTTPFS request is active.

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
The `curl_httpfs` fixes are under review in
[PR #75](https://github.com/dentiny/duckdb-curl-filesystem/pull/75) and
[PR #76](https://github.com/dentiny/duckdb-curl-filesystem/pull/76). The
HTTPFS-Curl timeout change is under review in
[`duckdb/duckdb-httpfs#490`](https://github.com/duckdb/duckdb-httpfs/pull/490).
Until those changes and the provider-specific POST fixes are merged, treat
these pinned source builds as compatibility-test inputs, not released support.

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
