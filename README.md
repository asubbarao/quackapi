# quackapi

QuackAPI serves HTTP routes whose handlers are DuckDB SQL. Request values bind
as prepared parameters, DuckDB types validate them, and query results become
HTTP responses.

```sql
INSTALL quackapi FROM community;
LOAD json;
LOAD quackapi;

CREATE ROUTE hello GET '/hello' AS SELECT 'world' AS message;
CREATE ROUTE item GET '/items/:id' AS SELECT $id::INTEGER AS id;

SELECT * FROM quackapi_serve(8000);
```

```sh
curl http://127.0.0.1:8000/hello
# [{"message":"world"}]

curl http://127.0.0.1:8000/items/42
# [{"id":42}]
```

Use a sequential session: load QuackAPI before its DDL so DuckDB has registered
the parser extension. QuackAPI targets DuckDB v1.5.6.

## Routes, authentication, and responses

`CREATE ROUTE` supports GET, POST, PUT, PATCH, DELETE, and HEAD. Path, query,
header, cookie, and request-body values bind through named SQL parameters.
Invalid casts and constraints return a structured 422 response. `CREATE AUTH`
defines API-key or HS256 JWT authentication, and `REQUIRE` applies it to a
route. `CREATE GROUP` adds a path prefix, default authentication, and OpenAPI
tags. OpenAPI, Swagger UI, ReDoc, static files, CORS, compression, query
deadlines, and response-size limits are served by the same DuckDB process.

Route responses support JSON, NDJSON, and CSV, with JSON as the default. Arrow
and Parquet are not HTTP response formats in this version.

Route authentication controls access to an endpoint. It does not filter rows
or rewrite arbitrary SQL by tenant. Put tenant predicates in the handler query
or use a scoped view.

## Outbound POST

`quackapi_post(url, body[, content_type[, headers]])` performs one POST for each
non-NULL URL row. Its result fields are `status`, `reason`, `body`, `headers`,
`error`, and `reused_connection`; all overloads are volatile. Load a supported
DuckDB HTTPUtil provider explicitly before use. The patched `curl_httpfs` and
HTTPFS-Curl providers are under upstream review; released community binaries
are not yet supported provider builds for this contract. QuackAPI does not
install providers automatically. See [provider status](docs/curl_httpfs.md).

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

The database-wide setting `quackapi_post_concurrency` bounds active requests.
`0` uses DuckDB's configured thread count (minimum one); positive values select
an explicit limit. A bounded waiting queue applies backpressure when workers
and queue slots are occupied. This schedules HTTP work only; it does not change
DuckDB's SQL transaction or database-file concurrency semantics.

For GET requests from SQL, use DuckDB's HTTP readers such as `read_text` and
`read_json` after loading the required reader extension. QuackAPI has no public
GET function or connection-pool diagnostics.

## Server controls

```sql
SELECT ready, listen_url FROM quackapi_wait(8000, 5000);
SELECT * FROM quackapi_routes();
SELECT * FROM quackapi_servers();
SELECT * FROM quackapi_stop(8000);
```

`quackapi_serve` binds to `127.0.0.1` by default. Use `host := '0.0.0.0'` only
when the routes and network boundary are configured for remote access. There
is no in-process TLS termination; use a reverse proxy for HTTPS termination.

DuckDB ordinary SQL concurrency rules still apply. Concurrent HTTP requests do
not turn one DuckDB database file into a multi-writer OLTP service. See
[Concurrency and limits](docs/RESOURCE_LIMITS.md).

## Removed surfaces

QuackAPI no longer includes its experimental durable job queue, row-access and
masking policies, SSE streams, GraphQL, native PostgreSQL execution, generated
table APIs, source importers, or Arrow/Parquet response encoders. The `radio`
extension is a separate message bus and does not provide the queue lease and
acknowledgement semantics formerly prototyped here. Arrow Flight is a separate
protocol; DuckDB's optional `duckflight` extension serves Flight SQL separately
and is not a mandatory QuackAPI dependency. See the
[migration guide](docs/MIGRATION.md) for SQL names and migration boundaries.

## Build and documentation

```sh
GEN=ninja make release
make test
bash test/conformance/run.sh
```

The build declares DuckDB JSON as an extension dependency. Load `json` before
starting a server that uses JSON routes; QuackAPI does not install it as a side
effect. `BODY SCHEMA` additionally requires `json_schema` to be available
locally before the route is created, and route DDL never installs it.

- [Guided tour](docs/index.md)
- [Function reference](docs/reference/functions.md)
- [DDL reference](docs/reference/ddl.md)
- [Route guide](docs/guide/routes-and-params.md)
- [Authentication](docs/guide/auth.md)
- [Outbound POST providers](docs/curl_httpfs.md)
- [Migration guide](docs/MIGRATION.md)
- [Community extension description](docs/community-page.md)
