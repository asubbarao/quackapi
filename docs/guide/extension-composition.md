# Compose SQL handlers with DuckDB extensions

QuackAPI routes run ordinary SQL in DuckDB. A handler can use tables, views,
scalar functions, and table functions already available in that database.
Loading a companion extension is an explicit deployment choice.

```text
HTTP client → QuackAPI route → prepared DuckDB SQL → response
                                   │
                                   ├── tables and views
                                   └── explicitly loaded extensions
```

## Read upstream data

Use DuckDB's HTTP readers inside a route after loading a reader provider such
as `httpfs` or `curl_httpfs`:

```sql
LOAD json;
LOAD httpfs;
LOAD quackapi;

CREATE ROUTE remote_status GET '/remote-status' AS
SELECT * FROM read_json('https://api.example.test/status');
```

QuackAPI does not install HTTP providers when a server starts. Reader behavior
and supported schemes come from the loaded DuckDB extension.

## Send an outbound POST

`quackapi_post` uses DuckDB's HTTPUtil provider and requires a POST-capable
provider to be explicitly loaded. `curl_httpfs` is recommended; `httpfs` is
also supported.

```sql
LOAD json;
LOAD curl_httpfs;
LOAD quackapi;

CREATE ROUTE submit POST '/submit' AS
SELECT (quackapi_post(
  'https://api.example.test/submit',
  json_object('id', $id::INTEGER)::VARCHAR,
  'application/json'
)).status AS upstream_status;
```

`quackapi_post` keeps one result row per input row, preserves its NULL and
volatile-function behavior, and reports status, response body, headers, and
transport errors. Configure `quackapi_post_concurrency` to bound simultaneous
requests. See [Outbound POST providers](../curl_httpfs.md).

## Keep route security explicit

`CREATE AUTH` and `REQUIRE` decide whether a caller may invoke a route. They do
not apply row filters to arbitrary SQL. Put tenant constraints in the handler
query or a scoped view and use DuckDB privileges for database access. There is
no built-in row-access or masking-policy rewrite.

## Protocol boundaries

QuackAPI currently serves ordinary bounded HTTP route responses in JSON,
NDJSON, and CSV. The extension does not host GraphQL, SSE, WebSocket, Arrow
Flight, or PostgreSQL protocols. DuckDB Flight/ADBC clients and the `radio`
message-bus extension can be composed separately, but they do not add those
protocols to the QuackAPI listener. In particular, `radio` does not implement
the durable queue lease, acknowledgement, and retry behavior of the removed
QuackAPI queue prototype.

See the [migration guide](../MIGRATION.md) for removed function and DDL names.
