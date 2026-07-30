# Thin GraphQL v0 (catalog-only)

Owner-driven, **not** months of PostGraphile. Built-in while `quackapi_serve` is running (also via `quackapi_request`); no `CREATE ROUTE` required for the global endpoint.

**Not DuckGQL.** [DuckGQL](https://duckgql.com/) is ISO GQL (graph analytics: `MATCH`, CSR, …). This surface is **GraphQL-over-HTTP** for tabular reads.

## Dual surface: GraphQL and HTTP at once

The database does not care which wire the client used. **Both resolve to SQL** in the same DuckDB session:

```text
  Client (GraphQL document)          Client (REST path/body)
           │                                  │
           ▼                                  ▼
  POST /graphql | GRAPHQL ROUTE      CREATE ROUTE / API FOR TABLE
           │                                  │
           ▼                                  ▼
   parse → table/cols                  bind $params
           │                                  │
           └──────────►  SQL  ◄───────────────┘
                          │
                     same catalog, same auth primitives,
                     same LOAD companions (tera, sitting_duck, …)
```

| Wire | You declare | Engine does |
|------|-------------|-------------|
| **HTTP** | `CREATE ROUTE … AS SELECT …` | Run that handler SQL |
| **GraphQL** | tables allowed (`FOR TABLE` / `GRAPHQL ROUTE … FROM`) | Map `{ users { id } }` → `SELECT "id" FROM "users" LIMIT n` |

Same tables can expose **both** in one serve — that is intentional, not a conflict:

```sql
CREATE TABLE users AS SELECT 1 AS id, 'ada' AS name;

CREATE ROUTE list_users GET '/users' AS
  SELECT id, name FROM users;

CREATE GRAPHQL ROUTE app POST '/gql' FROM users;

-- GET /users
-- POST /gql  {"query":"{ users { id name } }"}
```

Demo: [`examples/dual_surface.sql`](../../examples/dual_surface.sql).  
Optional AST→SQL recipe (sitting_duck, not the serve hot path): [`examples/graphql_ast_parse.sql`](../../examples/graphql_ast_parse.sql).

## What ships

| Surface | Behavior |
|---------|----------|
| **`POST /graphql`** | JSON body `{"query":"…"}` → GraphQL-ish result |
| **`GET /graphql/schema`** | Main-schema tables → column names (not full `__schema`) |
| **`CREATE GRAPHQL FOR TABLE`** | Optional **allowlist** for which tables the built-in endpoint exposes |
| **`CREATE GRAPHQL ROUTE`** | Named path mounts with per-route tables, optional auth + LIMIT |
| Auth | Built-in `/graphql` is **public**; named routes may `REQUIRE` a `CREATE AUTH` scheme |
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

## Named mounts: `CREATE GRAPHQL ROUTE`

First-class **named** GraphQL mounts (like `CREATE ROUTE`) for a private path, auth, or a subset of tables **without** switching the global endpoint:

```sql
CREATE GRAPHQL ROUTE public_api POST '/api/gql'
  FROM users, posts
  REQUIRE site
  LIMIT 50;

SELECT * FROM quackapi_graphql_routes();
-- name, method, path, tables, require_auth, limit

-- Query:
--   POST /api/gql          body {"query":"{ users { id } }"}
--   GET  /api/gql/schema   tables = FROM list only (mode=route)

DROP GRAPHQL ROUTE public_api;
```

| Piece | Rules |
|-------|--------|
| **name** | Registry id; unique; `OR REPLACE` overwrites |
| **METHOD** | **POST only** (v0) |
| **path** | Absolute `/…`; not `/graphql` or `/graphql/schema`; unique among GraphQL routes |
| **FROM tables** | Required ≥1; must exist in `main` at CREATE; **per-route only** (does not flip global allowlist) |
| **REQUIRE** | Optional `CREATE AUTH` scheme (auth scheme must exist at CREATE) |
| **LIMIT** | Optional row cap (default 100; range 1..100000) |

Independence:

- Built-in `POST /graphql` stays always-on; global allowlist via `CREATE GRAPHQL FOR TABLE` is separate.
- A GraphQL route never mutates `quackapi_graphql_tables()`.
- Unregistered path → normal 404.

**What stays v0** on any GraphQL surface until product need forces growth:

- No mutations / subscriptions  
- No nested joins / relation fields  
- No arguments, aliases, fragments, variables  
- No full GraphQL grammar / `__schema` introspection  

Alternative spelling rejected: `CREATE ROUTE x POST '/gql' AS GRAPHQL FROM …` (overloads ROUTE AS beyond SELECT). Dedicated noun matches `CREATE API FOR TABLE`.

## Future schema feeds

v0 discovers tables/columns from the **live DuckDB catalog**. **sitting_duck** already parses GraphQL (and app languages) as AST tables — use that offline to invent allowlists or richer types from source trees; wire into the thin execute path only when product needs it. **parser_tools** stays for SQL hygiene on generated SELECTs. Intentional debt: not a blocker for catalog-backed read APIs.

## Design stance

- **Dual surface:** REST and thin GraphQL side by side; both chew into queries.  
- Prefer `CREATE ROUTE` / `CREATE API FOR TABLE` when you own the SQL shape.  
- GraphQL v0 is a **read convenience** (column pick + multi-root) over the same tables.  
- **Allowlist** + **named routes** lock down exposure without closing open `/graphql` by default.  
- Grow grammar only when a concrete client needs args/filters — still as “→ one SELECT,” not Hasura-in-C++.
