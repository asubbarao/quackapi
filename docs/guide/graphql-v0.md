# Thin GraphQL v0 (catalog-only)

Owner-driven, **not** months of PostGraphile. Built-in while `quackapi_serve` is running (also via `quackapi_request`); no `CREATE ROUTE` required for the global endpoint.

**Not DuckGQL.** [DuckGQL](https://duckgql.com/) is ISO GQL (graph analytics: `MATCH`, CSR, …). This surface is **GraphQL-over-HTTP** for tabular reads.

## What ships

| Surface | Behavior |
|---------|----------|
| **`POST /graphql`** | JSON body `{"query":"…"}` → GraphQL-ish result |
| **`GET /graphql/schema`** | Main-schema tables → column names (not full `__schema`) |
| **`CREATE GRAPHQL FOR TABLE`** | Optional **allowlist** for which tables the built-in endpoint exposes |
| Auth | **Public** in v0 (require later) |
| Schema source | **DuckDB catalog only** (`duckdb_tables` / `duckdb_columns`) |

## Minimal query language

Only this shape (optional `query` keyword and optional operation name):

```graphql
query {
  tableName {
    col1
    col2
  }
}
```

or bare:

```graphql
{
  tableName {
    col1
    col2
  }
}
```

Multiple root fields are allowed (each becomes its own `SELECT`):

```graphql
{
  users { id name }
  posts { id title }
}
```

Each root field maps to:

```sql
SELECT "col1", "col2" FROM "tableName" LIMIT 100
```

Default row cap: **100** (`QUACKAPI_GRAPHQL_DEFAULT_LIMIT`).

### Response shape

Success:

```json
{
  "data": {
    "tableName": [
      {"col1": 1, "col2": "x"}
    ]
  }
}
```

Errors (GraphQL-ish; still HTTP 200 for document/execution issues; HTTP 400 only for unusable JSON body):

```json
{
  "errors": [
    {"message": "unknown table 'nope' (main schema catalog only)"}
  ]
}
```

### Explicitly **not** in v0

- Mutations / subscriptions  
- Nested joins / relation fields  
- Arguments, aliases, fragments, variables  
- Full GraphQL grammar  
- `__schema` / `__type` introspection (use `GET /graphql/schema`)  

## Call it

```sql
CREATE TABLE cases AS
SELECT 1 AS id, '24-000117' AS case_no;

SELECT * FROM quackapi_serve(8000);
```

```sh
curl -sS -X POST http://127.0.0.1:8000/graphql \
  -H 'Content-Type: application/json' \
  -d '{"query":"{ cases { id case_no } }"}'
# {"data":{"cases":[{"id":1,"case_no":"24-000117"}]}}

curl -sS http://127.0.0.1:8000/graphql/schema
# {"mode":"open","tables":{"cases":["id","case_no"]}, "note":"…"}
```

In-process (no TCP):

```sql
SELECT status, decode(body) FROM quackapi_request(
  'POST', '/graphql',
  '{"query":"{ cases { id case_no } }"}'
);
```

## Allowlist: `CREATE GRAPHQL FOR TABLE`

By default the built-in endpoint is **open**: every non-internal table/view in schema `main` is selectable (same as first v0).

Register one or more tables to switch into **allowlist** mode — only those names work on `POST /graphql` and appear in `GET /graphql/schema`:

```sql
CREATE GRAPHQL FOR TABLE users, posts;
-- CREATE OR REPLACE GRAPHQL FOR TABLE users;  -- idempotent

SELECT * FROM quackapi_graphql_tables();
-- mode=allowlist, table_name=posts|users

DROP GRAPHQL FOR TABLE posts;
DROP GRAPHQL ALL;   -- clear allowlist → open mode again
```

| Mode | When | `POST /graphql` | `GET /graphql/schema` |
|------|------|-----------------|------------------------|
| **open** | allowlist empty (default) | all main tables | all main tables |
| **allowlist** | ≥1 `CREATE GRAPHQL FOR TABLE` | registered only | registered only |

Does **not** mount a new path — still the global built-in `POST /graphql`. Tables must exist in `main` at CREATE time. Lifecycle matches other quackapi DDL (in-memory registry; re-declare after reopen).

### How this differs from the bare built-in

| | Built-in open | After `CREATE GRAPHQL FOR TABLE` |
|--|---------------|----------------------------------|
| Path | `/graphql` | same `/graphql` |
| Catalog | whole main schema | allowlist only |
| REST | — | still use `CREATE ROUTE` / `CREATE API FOR TABLE` for REST |

## Design: `CREATE GRAPHQL ROUTE` (not shipped)

First-class **named** GraphQL mounts (like `CREATE ROUTE`), for apps that want a private path, auth, or a subset of tables without switching the global endpoint:

```sql
-- Proposed (not implemented):
CREATE GRAPHQL ROUTE public_api POST '/api/gql'
  FROM users, posts
  [REQUIRE site]
  [LIMIT 50];

DROP GRAPHQL ROUTE public_api;
```

| Piece | Intent |
|-------|--------|
| **name** | Registry id (like routes); inspect later via a TF |
| **METHOD + path** | Mount only that path (global `/graphql` stays independent) |
| **FROM tables** | Per-route allowlist (does not force global allowlist) |
| **REQUIRE** | Reuse CREATE AUTH (global `/graphql` stays public in v0) |
| **LIMIT** | Optional per-route row cap |

**What stays v0** on any GraphQL surface until product need forces growth:

- No mutations / subscriptions  
- No nested joins / relation fields  
- No arguments, aliases, fragments, variables  
- No full GraphQL grammar / `__schema` introspection  

**vs built-in `POST /graphql`:** the built-in is always-on convenience; `CREATE GRAPHQL ROUTE` would be opt-in mounts with path/auth isolation. Alternative spelling considered and rejected for v0 work: `CREATE ROUTE x POST '/gql' AS GRAPHQL FROM …` (overloads ROUTE AS beyond SELECT). Prefer a dedicated noun next to `CREATE API FOR TABLE`.

## Future schema feeds

v0 discovers tables/columns from the **live DuckDB catalog**. Later, **sitting_duck** / **parser_tools** can feed types from external application code (models, ORMs) into a richer schema surface without changing the thin query path. That is intentional debt — not a blocker for catalog-backed read APIs.

## Design stance

- Prefer `CREATE ROUTE` / `CREATE API FOR TABLE` for typed REST.  
- GraphQL v0 is a **read convenience** over the same tables.  
- **Allowlist** is the first step toward first-class GraphQL DDL without closing the open default.  
- Grow only when a real product need forces grammar (filters, nested, auth) or named routes.
