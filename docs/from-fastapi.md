# Importing framework route and model metadata

The extension exposes two table functions:

```sql
LOAD quackapi;
```

```sql
SELECT method, path, handler_name, file, start_line, evidence
FROM quack_from_routes('fastapi', '/path/to/app');

SELECT model_name, field_name, field_type, is_required, is_optional,
       has_default, default_expr, file, field_line
FROM quack_from_models('fastapi', '/path/to/app');
```

The supported framework values are `fastapi`, `rails`, `express`, and `gin`.
Pass a source directory, file, or glob. The functions install/load the community
`sitting_duck` AST reader on first use. No Python, Ruby, Node, or Go application
runtime is executed to inspect the source.

These functions return **metadata rows**. They do not register routes, generate
a complete application, start a listener, execute framework dependencies, or
translate imperative handlers. Framework-specific SQL lives in `src/sql/` and
is embedded in the extension. The former eight public function names are
replaced by these two functions; see [migration notes](MIGRATION.md).

## Move an endpoint into DuckDB

1. Inspect the imported route/model rows and their source evidence.
2. Create the DuckDB tables or attach an external catalog used by the handler.
3. Write the handler as SQL and declare its request validation.
4. Register the route, then check its responses and `/openapi.json`.

For a FastAPI `GET /items/{id}` endpoint backed by an `items` table:

```sql
CREATE ROUTE item GET '/items/:id' AS
SELECT * FROM items WHERE id = $id::INTEGER;

SELECT * FROM quackapi_request('GET', '/items/42');
```

Use [PARAM constraints](guide/routes-and-params.md),
[BODY TYPE / BODY SCHEMA](guide/request-bodies.md),
[REST auth](guide/auth.md), and [row policies](guide/policies.md) to express
the application's contracts. Imported field types are source metadata; review
their mapping before using them as DuckDB types or JSON Schema.

## Pydantic required fields

FastAPI model extraction distinguishes nullability from having a default:

| Source declaration | `has_default` | `is_required` |
|--------------------|---------------|---------------|
| `name: str` | false | true |
| `name: str = Field()` | false | true |
| `name: str = Field(description="name")` | false | true |
| `name: str = Field(description="name", default=...)` | false | true |
| `name: str = Field(default="guest")` | true | false |

Custom validators, default factories, dependency injection, sessions, and
external SDK calls still require application logic. Extracting a factory's
presence does not execute it. Queue workers or loaded extensions can handle
side effects; WebSockets use [CREATE STREAM … WS](guide/stream.md#websocket).

## Source and evidence

- [Bridge implementation and fixtures](../bridges/from_x/README.md)
- `test/sql/quackapi_from_x_bridge.test` and the framework-specific SQL suites
- `test/sql/quackapi_pydantic_contract.test` for required/default semantics

Original corpus studies in `bridges/from_x/docs/` are historical experiments.
Their temporary scripts, coverage percentages, and end-to-end facade results
are not the contract of the current import functions.
