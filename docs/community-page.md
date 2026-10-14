# QuackAPI community extension page

This source-verified text complements the root [`description.yml`](../description.yml).
The current supported SQL surface is documented in the
[function reference](reference/functions.md), [DDL reference](reference/ddl.md),
and [migration guide](MIGRATION.md).

## Overview

QuackAPI serves HTTP routes whose handlers are DuckDB SQL. Route DDL declares
typed inputs and response formats; API-key and HS256 JWT authentication can
protect routes. Route groups add a path prefix, inherited authentication, and
OpenAPI tags. The same process serves OpenAPI, Swagger UI, ReDoc, static files,
CORS, compression, and bounded SQL responses.

```sql
LOAD json;
LOAD quackapi;

CREATE ROUTE hello GET '/hello' AS SELECT 'world' AS message;
SELECT * FROM quackapi_serve(8000);
```

Route handlers use prepared parameters. Authentication controls access to a
route; it does not add database-wide row filtering or rewrite arbitrary SQL by
tenant. Write tenant predicates into handler SQL or use explicitly scoped
views.

## Outbound POST

`quackapi_post(url, body[, content_type[, headers]])` performs volatile HTTP
and HTTPS POSTs through DuckDB's HTTPUtil provider. `curl_httpfs` is
recommended and `httpfs` is also supported. Load a provider explicitly; the
extension does not install one. Results preserve input row alignment and
contain status, reason, body, headers, transport error, and connection-reuse
metadata. `quackapi_post_concurrency` bounds simultaneous requests for a
database instance.

For GET requests, use DuckDB's HTTP reader functions. QuackAPI has no public
fetch or pool-diagnostics API.

## Scope

The HTTP response formats are JSON, NDJSON, and CSV. QuackAPI does not provide
the former experimental queue, row-access or masking policy, SSE, GraphQL,
generated table/source-import, native PostgreSQL, Arrow, or Parquet surfaces.
`radio` is a separate message bus; it does not provide the removed queue's
lease, acknowledgement, or retry semantics. Arrow Flight and ADBC are separate
client/protocol choices and are not QuackAPI runtime dependencies. DuckDB's
ordinary SQL transaction and database-file concurrency behavior still applies.

Build target: DuckDB v1.5.6. The extension declares DuckDB JSON as a build
dependency; load JSON before serving routes that use it. `BODY SCHEMA` also
requires `json_schema` to be available locally. Neither is installed by
QuackAPI as a side effect.
