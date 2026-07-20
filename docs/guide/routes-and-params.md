# Routes, typed params, and 422

You already know routes. In quackapi a route is a **named** HTTP endpoint whose handler is **SQL**.

All examples below were run against `build/release/duckdb -unsigned` with `LOAD quackapi;`.

---

## Create a route

```sql
CREATE ROUTE hello GET '/hello' AS SELECT 'world' AS msg;
```

```sh
curl http://127.0.0.1:8000/hello
# [{"msg":"world"}]
# HTTP 200
```

### What each piece means

| Piece | Role |
|-------|------|
| `hello` | Registry name (for `DROP ROUTE` / inspect) |
| `GET` | HTTP method |
| `'/hello'` | Path pattern (must start with `/` unless the route is in a [GROUP](groups.md)) |
| `AS SELECT …` | Handler — any SQL that returns a result |

Methods: `GET`, `POST`, `PUT`, `DELETE`, `PATCH`, `HEAD`.

---

## Path parameters

Use `:name` or `{name}` in the pattern. The value binds as `$name`.

```sql
CREATE ROUTE item GET '/items/:id' AS
SELECT $id::INTEGER AS id;
```

```sh
curl http://127.0.0.1:8000/items/42
# [{"id":42}]

curl http://127.0.0.1:8000/items/abc
# {"detail":[{"loc":["path","id"],"msg":"Input should be a valid integer","type":"type_error"}]}
# HTTP 422
```

Nested paths work the same way:

```sql
CREATE ROUTE get_post GET '/users/:id/posts/:post_id' AS
SELECT $id::INTEGER AS user_id, $post_id::INTEGER AS post_id;
```

---

## Query parameters

Any `$name` the handler needs that is **not** a path capture is taken from the query string (or body — see [request bodies](request-bodies.md)).

```sql
CREATE TABLE users AS
SELECT * FROM (VALUES
  (1, 'alice', 30),
  (2, 'bob', 25),
  (3, 'carol', 40)
) t(id, name, age);

CREATE ROUTE search GET '/search' AS
SELECT id, name, age FROM users
WHERE name ILIKE $q::VARCHAR || '%'
ORDER BY id;
```

```sh
curl 'http://127.0.0.1:8000/search?q=a'
# [{"id":1,"name":"alice","age":30}]
```

Missing required params (no default) → **422**.

---

## PARAM: types, defaults, constraints

`PARAM` is the explicit FastAPI-`Query`-style surface. Use it when you want defaults or bounds without relying only on casts.

```sql
CREATE ROUTE search_limit GET '/search_limit'
  PARAM limit INTEGER DEFAULT 10 LE 100
  AS
SELECT id, name, age FROM users
WHERE name ILIKE $q::VARCHAR || '%'
ORDER BY id
LIMIT $limit::INTEGER;
```

| Request | Result |
|---------|--------|
| `?q=a` (no limit) | **200** — default `10` applies |
| `?q=a&limit=2` | **200** — two rows max |
| `?q=a&limit=101` | **422** `less_than_equal` |
| `?q=a&limit=abc` | **422** `type_error` |
| `?q=a&limit=1.5` | **422** (strict int — no silent float) |
| `?q=a&limit=1e2` | **422** (scientific notation rejected for int) |
| `?q=a&limit=-1` | **200** `[]` (SQL LIMIT, never a 500) |

Live:

```sh
curl 'http://127.0.0.1:8000/search_limit?q=a'
# [{"id":1,"name":"alice","age":30}]

curl 'http://127.0.0.1:8000/search_limit?q=a&limit=101'
# {"detail":[{"loc":["query","limit"],"msg":"Input should be less than or equal to 100","type":"less_than_equal"}]}
```

### PARAM grammar (summary)

```
PARAM <name> [<type>] [HEADER|COOKIE|QUERY [wire-name]]
      [DEFAULT <literal>]
      [GE|GT|LE|LT <n>]
      [MIN_LENGTH|MAX_LENGTH <n>]
```

Types accepted (aliases in parentheses):  
`INTEGER` (`INT`), `BIGINT`, `VARCHAR` (`TEXT`, `STRING`), `BOOLEAN` (`BOOL`), `DOUBLE`, `FLOAT` (`REAL`), `HUGEINT`, `UBIGINT`, `UINTEGER`.

Constraints:

| Clause | Meaning (FastAPI-shaped error type) |
|--------|-------------------------------------|
| `GE n` | ≥ n → `greater_than_equal` |
| `GT n` | > n → `greater_than` |
| `LE n` | ≤ n → `less_than_equal` |
| `LT n` | < n → `less_than` |
| `MIN_LENGTH n` / `MAX_LENGTH n` | string length |

Header/cookie sources are covered in [headers & cookies](headers-cookies-redirects.md).

---

## The 422 shape (memorize this)

Every client input failure uses the same envelope:

```json
{
  "detail": [
    {
      "loc": ["path" | "query" | "header" | "cookie" | "body", "<name>"],
      "msg": "human-readable message",
      "type": "type_error | less_than_equal | missing | …"
    }
  ]
}
```

| `loc[0]` | Where the bad value came from |
|----------|--------------------------------|
| `path` | Path segment (`:id`) |
| `query` | Query string |
| `header` | Request header |
| `cookie` | Cookie |
| `body` | JSON / form / multipart / schema |

This matches FastAPI’s `RequestValidationError` shape so existing client code often just works.

---

## Strict integers

Path and query integers reject floats and scientific notation:

```sh
curl http://127.0.0.1:8000/users/1.5
# {"detail":[{"loc":["path","id"],"msg":"Input should be a valid integer","type":"type_error"}]}
# HTTP 422
```

Leading zeros are fine (`/users/01` → id `1`).

---

## Status codes on success

Default success status is **200**. Override with `STATUS`:

```sql
CREATE ROUTE create_user POST '/users' STATUS 201 AS
SELECT $name::VARCHAR AS name, $age::INTEGER AS age;
```

```sh
curl -X POST 'http://127.0.0.1:8000/users?name=dave&age=35' \
  -H 'Content-Type: application/json' -d '{}'
# [{"name":"dave","age":35}]
# HTTP 201
```

(JSON body binding for the same route is in [request bodies](request-bodies.md).)

---

## 404, 405, trailing slash

```sh
curl http://127.0.0.1:8000/nope
# {"detail":"Not Found"}
# HTTP 404

curl -X POST http://127.0.0.1:8000/health -H 'Content-Type: application/json' -d '{}'
# {"detail":"Method Not Allowed"}
# HTTP 405
# Allow: GET, HEAD

curl -sI http://127.0.0.1:8000/health/
# HTTP/1.1 307
# Location: /health
```

- Wrong method → **405** with an **`Allow`** header.
- GET routes auto-answer **HEAD** (empty body).
- Extra trailing slash → **307** to the registered form (Starlette-style).
- OPTIONS without CORS → **405** + `Allow` (same as FastAPI without middleware). With CORS enabled, OPTIONS can return **204** preflight — see serve `cors_origins` in [functions reference](../reference/functions.md).

---

## Inspect and drop

```sql
SELECT name, method, pattern, status, handler, require_auth
FROM quackapi_routes();

DROP ROUTE hello;
CREATE OR REPLACE ROUTE hello GET '/hello' AS SELECT 'again' AS msg;
```

`CREATE OR REPLACE` does not leave a half-applied route if validation fails.

---

## Next

- [Request bodies](request-bodies.md) — JSON, form, multipart, BODY SCHEMA  
- [Headers, cookies, redirects](headers-cookies-redirects.md)
