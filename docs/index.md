# QuackAPI documentation

QuackAPI registers HTTP routes in DuckDB and executes each handler as SQL.
Start with the [README quickstart](../README.md), then use these guides for the
retained API.

## Getting started

- [Routes and typed parameters](guide/routes-and-params.md)
- [Request bodies](guide/request-bodies.md)
- [Authentication](guide/auth.md)
- [Groups](guide/groups.md)
- [Static files](guide/static-files.md)
- [Compression](guide/compression.md)
- [OpenAPI](guide/openapi.md)

## Reference

- [Function reference](reference/functions.md)
- [DDL reference](reference/ddl.md)
- [Middleware](MIDDLEWARE.md)
- [Request budgets and limits](RESOURCE_LIMITS.md)
- [Outbound POST providers](curl_httpfs.md)
- [Removed APIs and migration](MIGRATION.md)

## Scope and operating limits

The current HTTP response formats are JSON, NDJSON, and CSV. Route
authentication does not enforce row-level policies on arbitrary SQL. DuckDB's
ordinary SQL and database-file concurrency rules apply to requests; the HTTP
server is not a multi-writer OLTP tier. Arrow Flight remains a separate
protocol.

Historical feature research in this repository predates the API cleanup. Use
the [migration guide](MIGRATION.md) and current references above for supported
behavior.
