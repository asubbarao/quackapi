# OpenAPI, Swagger UI, and ReDoc

Every running server exposes a live OpenAPI 3.1 document and two interactive UIs. You do not register these routes yourself — they are built in.

All examples run against `build/release/duckdb -unsigned` with `LOAD quackapi;`.

---

## Endpoints

| Path | What you get |
|------|----------------|
| `GET /openapi.json` | OpenAPI **3.1** document from the live route + auth registries |
| `GET /docs` | **Swagger UI** |
| `GET /redoc` | **ReDoc** |

These paths do **not** appear in `quackapi_routes()` (by design).

---

## Live check

```sql
CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;
SELECT * FROM quackapi_serve(8000);
```

```sh
curl -s http://127.0.0.1:8000/openapi.json | head -c 200
# {"openapi":"3.1.0", …}

curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8000/docs
# 200  (HTML with swagger)

curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8000/redoc
# 200  (HTML with redoc + /openapi.json)
```

Open in a browser:

- http://127.0.0.1:8000/docs  
- http://127.0.0.1:8000/redoc  

---

## What gets documented

Generated from:

- `quackapi_routes()` — paths, methods, params, status, tags, security  
- `quackapi_auths()` — `components.securitySchemes`  
- Group tags (from [CREATE GROUP](groups.md))

Path params in patterns use OpenAPI brace form (`/items/{id}`) in the document even when you registered `:id`.

---

## Tips

- Create routes **before or after** serve — the document always reflects the current registry.  
- Use groups’ `tags=` so Swagger groups endpoints by version or domain.  
- `REQUIRE` auth surfaces as security requirements on those operations.

---

## Next

- [DDL reference](../reference/ddl.md)  
- [FastAPI parity](../fastapi-parity.md)
