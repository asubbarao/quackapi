# SQL middleware

QuackAPI middleware runs parameterized SQL around a matched route. It is useful
for request IDs, security headers, timing, audit inserts with `RETURNING`, and
central rejection rules without duplicating them in every route.

```sql
CREATE MIDDLEWARE request_id BEFORE AS
  SELECT true AS allow,
         NULL::INTEGER AS status,
         NULL::VARCHAR AS body,
         'X-Request-Id' AS header_name,
         $request_id AS header_value;

CREATE MIDDLEWARE tenant_guard BEFORE GROUP api_v1 AS
  SELECT $auth_subject IS NOT NULL AS allow,
         401::INTEGER AS status,
         '{"detail":"Authentication required"}' AS body,
         NULL::VARCHAR AS header_name,
         NULL::VARCHAR AS header_value;

CREATE MIDDLEWARE timing AFTER AS
  SELECT true AS allow,
         NULL::INTEGER AS status,
         NULL::VARCHAR AS body,
         'Server-Timing' AS header_name,
         'app;dur=' || $elapsed_ms::VARCHAR AS header_value;
```

The grammar is:

```sql
CREATE [OR REPLACE] MIDDLEWARE name BEFORE|AFTER [GROUP group_name] AS sql;
DROP MIDDLEWARE name;
```

Middleware names and group names use letters, digits, `_`, and `-`. Definitions
are database-scoped, survive separately-loaded extension copies in the same
database, and disappear when that database closes. Inspect the active
definitions with `SELECT * FROM quackapi_middlewares()`.

`BEFORE` runs after route matching and authentication, then before the route
handler. `AFTER` runs after the handler produces its response. A route group's
middleware applies only to member routes. Within a scope, creation order is
stable and `CREATE OR REPLACE` preserves a definition's position. The full
order is global then group for `BEFORE`, and group then global for `AFTER`.
This first release wraps ordinary `CREATE ROUTE` handlers and their in-process
`quackapi_request` calls. Built-in health endpoints, named GraphQL routes, the
built-in GraphQL endpoint, and SSE streams keep their dedicated execution
paths and do not run SQL middleware yet.

QuackAPI binds request data through DuckDB prepared-statement parameters. It
never builds SQL from an HTTP value. Both phases may use `$request_id`,
`$method`, `$path`, `$route`, `$group`, `$client_ip`, `$auth_subject`,
`$headers_json`, `$query_json`, `$body`, and verified `$claims_<claim>` values.
`AFTER` also receives `$status` and `$elapsed_ms`. Unknown parameters fail the
request rather than reading an undeclared request value. Missing optional
values and missing claims bind as SQL `NULL`.

Rows returned from middleware must have an `allow BOOLEAN` column. `allow =
false` stops the lifecycle and returns `status INTEGER` (default 403) and
`body VARCHAR` (default rejection JSON). `status` and `body` must be NULL when
`allow` is true. Optional `header_name VARCHAR` and `header_value VARCHAR`
add a response header and must occur together. A statement that produces zero
rows is a no-op; for an audit write that affects a response, use `RETURNING`
to return the control columns.

Output is deliberately bounded: 64 definitions per database, 16 KiB SQL per
definition, 64 returned rows per invocation, 8 KiB per response header, and a
1 MiB blocking body. Invalid control types, duplicate control columns, unsafe
header names, CR/LF header values, hop-by-hop headers, SQL errors, policy
rewrite failures, and deadline expiry fail closed with a sanitized response.
Middleware SQL uses the same row-access and masking-policy rewrite as route
SQL. Authentication always runs before middleware, so a middleware cannot
bypass a route's auth requirement or manufacture claims.
