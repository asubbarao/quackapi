# DDL reference

Load QuackAPI before executing its DDL so DuckDB has registered the parser
extensions. Load DuckDB's JSON extension before QuackAPI when the database will
serve JSON routes or use JSON body validation:

```sql
LOAD json;
LOAD quackapi;
```

## `CREATE ROUTE`

```sql
CREATE [OR REPLACE] ROUTE <name> <method> '<path>'
  [STATUS <status_code>]
  [FORMAT json | ndjson | csv]
  [EMPTY STATUS <status_code>]
  [REQUIRE <auth_name>]
  [GROUP <group_name>]
  [SCHEDULE '<six-field-cron>']
  [BODY SCHEMA '<json-schema>']
  [BODY TYPE <duckdb-type>]
  [PARAM <name> <duckdb-type>]
AS <select_statement>;

DROP ROUTE <name>;
```

Supported methods are GET, POST, PUT, PATCH, DELETE, and HEAD. Paths begin
with `/`; named path components such as `'/items/:id'` bind to `$id` in the
handler. Query parameters, selected headers/cookies, and request-body values
can also be declared as handler parameters. Values are bound as prepared SQL
parameters. See the [route guide](../guide/routes-and-params.md) for binding
details.

The default response is JSON. `FORMAT` supports only `json`, `ndjson`, and
`csv`; it does not encode Arrow or Parquet. `STATUS` selects the response
status for a successful handler. `EMPTY STATUS` selects the status used when
the handler returns no rows. `BODY SCHEMA` validates a JSON request body and
requires the DuckDB `json` and `json_schema` extensions to be available; route
DDL does not install them. `BODY TYPE` and `PARAM` declare typed inputs.

```sql
CREATE ROUTE item GET '/items/:id' STATUS 200 AS
  SELECT $id::INTEGER AS id;

CREATE ROUTE create_item POST '/items'
  BODY TYPE JSON
  BODY SCHEMA '{"type":"object","required":["name"]}'
AS
  SELECT json_extract_string($body, '$.name') AS name;
```

`SCHEDULE` accepts a six-field cron expression with seconds first. Scheduled
routes cannot contain path parameters.

## `CREATE AUTH`

```sql
CREATE [OR REPLACE] AUTH <name> AS API_KEY
  [(HEADER '<header-name>')];

CREATE [OR REPLACE] AUTH <name> AS JWT
  (SECRET '<secret>' [, ALGORITHM HS256]);

DROP AUTH <name>;
```

API keys default to the `X-API-Key` header. Add keys with
`quackapi_add_api_key`; key inspection never returns the secret value. JWT
authentication supports HS256. Apply an auth definition with `REQUIRE` on a
route or as a group's inherited auth.

```sql
CREATE AUTH api AS API_KEY;
SELECT * FROM quackapi_add_api_key('api', 'secret-key', 'user-42');
CREATE ROUTE me GET '/me' REQUIRE api AS SELECT $claims_sub AS subject;
```

Route auth decides whether a request may reach its handler. It does not filter
rows or rewrite arbitrary SQL by tenant. Put tenant predicates directly in
the handler or use a scoped view.

## `CREATE GROUP`

```sql
CREATE [OR REPLACE] GROUP <name>
  WITH (prefix = '<path-prefix>' [, auth = <auth-name>]
       [, tags = '<tag>[, <tag>...]']);

DROP GROUP <name>;
```

A group prefixes route paths and can provide default auth and OpenAPI tags.
A route's explicit `REQUIRE` takes precedence over inherited group auth.

```sql
CREATE GROUP api_v1 WITH (prefix = '/api/v1', auth = api, tags = 'v1,public');
CREATE ROUTE status GET '/status' GROUP api_v1 AS SELECT 'ok' AS status;
```

## `CREATE MIDDLEWARE`

```sql
CREATE [OR REPLACE] MIDDLEWARE <name>
  BEFORE | AFTER [GROUP <group-name>]
AS <select_statement>;

DROP MIDDLEWARE <name>;
```

Middleware is SQL evaluated at its declared phase. The query must return the
columns required by that phase; invalid or failed middleware blocks the
request. Authentication checks for a protected route happen before its
middleware. See the [middleware guide](../MIDDLEWARE.md).

```sql
CREATE MIDDLEWARE request_gate BEFORE AS
  SELECT true AS allow;
```

The former queue, row-access/masking policy, stream, GraphQL, table-generation,
and native PostgreSQL DDL are not part of the current extension. See the
[migration guide](../MIGRATION.md).
