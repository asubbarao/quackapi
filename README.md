# quackapi

HTTP JSON APIs from SQL, inside DuckDB. Routes are DDL, handlers are SELECTs,
and validation comes from the database's own type system.

```sql
LOAD quackapi;

CREATE ROUTE hello GET '/hello' AS SELECT 'world' AS msg;
CREATE ROUTE item  GET '/items/:id' AS SELECT $id::INT AS id;

SELECT * FROM quackapi_serve(8000);
```

```
$ curl http://127.0.0.1:8000/hello
[{"msg":"world"}]

$ curl http://127.0.0.1:8000/items/42
[{"id":42}]

$ curl http://127.0.0.1:8000/items/abc
{"detail":[{"loc":["path","id"],"msg":"Input should be a valid INTEGER","type":"type_error"}]}
```

No app server, no ORM, no schema duplication: the query already types the
response, so the framework doesn't need a model layer to re-declare it.

## API

| Surface | What it does |
|---|---|
| `CREATE [OR REPLACE] ROUTE <name> <METHOD> '<pattern>' [STATUS <n>] AS <select>` | Register an endpoint. `:param` / `{param}` path segments and query-string params bind to the handler's named parameters (`$param`). |
| `DROP ROUTE <name>` | Remove an endpoint. Changes apply live while serving. |
| `quackapi_serve([port], host := '127.0.0.1')` | Serve on background threads; the shell stays usable. |
| `quackapi_stop([port])` | Stop one server, or all. |
| `quackapi_routes()` | Inspect the registry. |
| `quackapi_servers()` | List running servers. |

## Behavior

- **Validation**: request params are cast to the types the prepared handler
  expects; failures return `422` with a FastAPI-shaped `detail` body. Missing
  required params → `422`; unknown path → `404`; wrong method → `405`.
- **Responses**: JSON array of row objects. JSON types follow column types —
  numbers stay numbers, booleans stay booleans, lists/structs nest, `NULL` is
  `null`.
- **State**: the route registry lives with the database instance — nothing is
  written to your catalog. Serving uses DuckDB's bundled httplib (the same
  transport as the core `quack` RPC extension) with a listener thread and
  worker pool.

## Build

```sh
git clone --recurse-submodules https://github.com/asubbarao/quackapi
cd quackapi
GEN=ninja make release
build/release/duckdb   # extension is pre-loaded in this shell
```

Run tests: `make test`

## License

MIT
