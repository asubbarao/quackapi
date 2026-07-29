# Thin GraphQL v0 (catalog-only)

Owner-driven, **not** months of PostGraphile. Built-in while `quackapi_serve` is running; no `CREATE ROUTE` required.

## What ships

| Surface | Behavior |
|---------|----------|
| **`POST /graphql`** | JSON body `{"query":"…"}` → GraphQL-ish result |
| **`GET /graphql/schema`** | Main-schema tables → column names (not full `__schema`) |
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
# {"tables":{"cases":["id","case_no"]}, "note":"…"}
```

## Future schema feeds

v0 discovers tables/columns from the **live DuckDB catalog**. Later, **sitting_duck** / **parser_tools** can feed types from external application code (models, ORMs) into a richer schema surface without changing the thin query path. That is intentional debt — not a blocker for catalog-backed read APIs.

## Design stance

- Prefer `CREATE ROUTE` / `CREATE API FOR TABLE` for typed REST.  
- GraphQL v0 is a **read convenience** over the same tables.  
- Grow only when a real product need forces grammar (filters, nested, auth).
