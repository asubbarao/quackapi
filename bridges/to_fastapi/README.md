# `quack_to_fastapi` — a quackapi route as FastAPI

The inverse of [`quack_from_x`](../from_x/README.md). A quackapi route already serves an HTTP contract:
its pattern and parameters are the request, its typed result columns are the response. These two SQL
files read that contract from the running server and write the FastAPI version of the route, with no
hand-written DTO.

| Piece | Comes from |
|---|---|
| `<route>_dto.py`, Pydantic models | `DESCRIBE` of the handler with its parameters bound to NULL, then of each struct it returns; ENUM values from `enum_range`; a field is required when every source column of that name and type is NOT NULL in `information_schema` and `SUMMARIZE` of the live route finds no null at the sample URLs |
| `<route>_route.py`, the FastAPI route | method, envelope and handler from `quackapi_routes()`; path parameters from the route's own pattern (`urlpattern`); query parameter types from the server's `/openapi.json` |
| `<route>.sql`, what that route runs | the handler, unchanged except for any renamed catalogs, returning the body as one BLOB |
| `<Route>Api.ts`, a typed client | the same fields as the DTO, as TypeScript interfaces |

So the Python and TypeScript types are DuckDB's own: `DECIMAL(12,2)` is `Decimal`, an ENUM is a
`Literal`, a list of structs is a list of its own model.

## Run

On the quackapi server that serves the route, with `urlpattern`, `netquack` and `tera` loaded and `jq`
on `PATH`:

1. Set `wanted_route` in `route_columns.sql`: the route, the catalogs it reads and a few sample URLs.
   The URLs give the server (netquack) and the live rows SUMMARIZE checks. Run it.
2. Set `wanted_route` in `quack_to_fastapi.sql`: one sample URL, the templates and the output folder.
   Run it; the last statement formats the Python with ruff.

`templates/fastapi/` is plain FastAPI + duckdb: the route runs `setup.sql` (yours: attach what the
route reads, under the same catalog names) and then the route's SQL. A project with its own auth,
database attach or generated client writes its own `*.tera` files over the same context; keys it needs
beyond the context go in `wanted_route.extras`, and `catalog_renames` maps the server's catalog names
to the project's.

## Example

`example/orders_route.sql` creates two small tables and the route
`GET /customers/:customer_id/orders?since=`. `example/app/` is what the two files generated from it,
plus the hand-written `setup.sql` and `__init__.py`. `example/test_app.py` mounts the generated route in
FastAPI and checks it serves the body the quackapi route served for the same URL
(`quackapi_response.json`):

```bash
cd bridges/to_fastapi/example
uv run --no-project --with fastapi --with httpx --with duckdb --with pytest pytest -q test_app.py
npx -p typescript tsc --noEmit --strict --target es2022 --lib es2022,dom app/ExampleOrdersApi.ts
```

## Limits

- A field the handler computes (`coalesce(x, [])`) has no source column, so it stays optional even
  when it is never null.
- Source columns are matched by name and type in the tables the handler names, not by lineage.
- Structs are described three levels deep.
- Every route must be named in `wanted_route`; the files take one route per run.
