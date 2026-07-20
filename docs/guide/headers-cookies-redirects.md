# Headers, cookies, redirects, status codes, content types

All examples run against `build/release/duckdb -unsigned` with `LOAD quackapi;`.

---

## Header parameters

Declare a header with `PARAM … HEADER`. Underscores in the SQL name become hyphens on the wire (`x_token` → `X-Token`). Lookup is case-insensitive.

```sql
CREATE ROUTE header_echo GET '/header-echo'
  PARAM x_token HEADER
  AS
SELECT $x_token::VARCHAR AS token;
```

```sh
curl http://127.0.0.1:8000/header-echo -H 'X-Token: abc'
# [{"token":"abc"}]

curl http://127.0.0.1:8000/header-echo -H 'x-token: lower'
# [{"token":"lower"}]

curl http://127.0.0.1:8000/header-echo
# {"detail":[{"loc":["header","x_token"], … "type":"missing" …}]}
# HTTP 422
```

### Explicit wire name

```sql
CREATE ROUTE ua GET '/ua'
  PARAM agent VARCHAR HEADER 'User-Agent'
  AS
SELECT $agent::VARCHAR AS agent;
```

```sh
curl http://127.0.0.1:8000/ua -H 'User-Agent: quackapi-test/1.0'
# agent contains quackapi-test/1.0
```

---

## Cookie parameters

```sql
CREATE ROUTE profile GET '/profile'
  PARAM session COOKIE
  AS
SELECT $session::VARCHAR AS session;
```

```sh
curl http://127.0.0.1:8000/profile -H 'Cookie: session=sess-abc'
# [{"session":"sess-abc"}]

curl http://127.0.0.1:8000/profile -H 'Cookie: other=1; session=xyz; flag=true'
# [{"session":"xyz"}]

curl http://127.0.0.1:8000/profile
# loc=["cookie","session"] — HTTP 422
```

---

## Set-Cookie on the response

Project a column named `set_cookie` (or `set-cookie`). It becomes a `Set-Cookie` header and is **removed** from the JSON body.

```sql
CREATE ROUTE login POST '/login' AS
SELECT 'session=sess-abc; Path=/' AS set_cookie, true AS ok;
```

```sh
curl -i -X POST http://127.0.0.1:8000/login \
  -H 'Content-Type: application/json' -d '{}'
# HTTP 200
# Set-Cookie: session=sess-abc; Path=/
# [{"ok":true}]
```

---

## Redirects

Use `STATUS` in the 3xx range and a `location` column:

```sql
CREATE ROUTE old_home GET '/old-home' STATUS 307 AS
SELECT '/new-home' AS location;
```

```sh
curl -i http://127.0.0.1:8000/old-home
# HTTP 307
# Location: /new-home
# (empty body when location is the only control column)
```

Extra data columns still appear in the body; `location` is stripped:

```sql
CREATE ROUTE moved GET '/moved' STATUS 301 AS
SELECT 'https://example.com/new' AS location, 'gone' AS note;
```

```sh
curl -i http://127.0.0.1:8000/moved
# HTTP 301
# Location: https://example.com/new
# [{"note":"gone"}]
```

---

## Status codes

```sql
CREATE ROUTE status_created GET '/status/created' STATUS 201 AS
SELECT 'created' AS text;

CREATE ROUTE status_teapot GET '/status/teapot' STATUS 418 AS
SELECT 'short' AS text;

CREATE ROUTE status_nocontent GET '/status/nocontent' STATUS 204 AS
SELECT '' AS text;
```

Valid range for `STATUS` is **100–599**. Default is **200**.

---

## Content types via column names

### JSON (default)

Multi-column (or non-magic single column) → `application/json` array of objects. Types follow DuckDB:

```sql
CREATE ROUTE page_json GET '/json' AS
SELECT 'world' AS msg, 42 AS n, true AS ok, NULL::INTEGER AS missing;
```

```sh
curl http://127.0.0.1:8000/json
# [{"msg":"world","n":42,"ok":true,"missing":null}]
# Content-Type: application/json
```

### HTML

Single column named `html`:

```sql
CREATE ROUTE page_html GET '/page' AS
SELECT '<h1>hi</h1>' AS html;
```

```sh
curl -i http://127.0.0.1:8000/page
# Content-Type: text/html; charset=utf-8
# <h1>hi</h1>
```

### Plain text

Single column named `text`:

```sql
CREATE ROUTE page_text GET '/plain' AS
SELECT 'hello' AS text;
```

```sh
curl -i http://127.0.0.1:8000/plain
# Content-Type: text/plain; charset=utf-8
# hello
```

---

## CORS (optional)

CORS is **off** by default. Enable with a setting or a serve named parameter:

```sql
SET quackapi_cors_origins = '*';
-- or: SET quackapi_cors_origins = 'https://app.example,https://admin.example';

SELECT * FROM quackapi_serve(8000, cors_origins := '*');
```

With CORS on, OPTIONS preflight returns **204** and CORS headers. Without CORS, OPTIONS on a normal route returns **405** + `Allow` (FastAPI default without middleware).

---

## Next

- [Auth](auth.md)  
- [OpenAPI](openapi.md)
