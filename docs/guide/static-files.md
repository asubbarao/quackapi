# Static files (`static_dir`)

Unrouted **GET** requests can be served from a directory on disk. Think FastAPI/Starlette `StaticFiles` mount at `/` for leftovers after routes.

All examples run against `build/release/duckdb -unsigned` with `LOAD quackapi;`.

---

## Serve with a static directory

```sql
CREATE ROUTE health GET '/health' AS SELECT 'ok' AS status;

SELECT * FROM quackapi_serve(
  8000,
  host := '127.0.0.1',
  static_dir := './static'
);
```

Put files under `./static`:

```text
static/
  hi.txt
  app.js
  index.html
```

```sh
# assumes ./static/hi.txt contains: hello-static
curl http://127.0.0.1:8000/hi.txt
# hello-static
# HTTP 200
```

Live check (absolute path):

```sql
SELECT * FROM quackapi_serve(8000, static_dir := '/tmp/my_static');
```

---

## Precedence

1. **Registered routes** (and streams) win.  
2. Built-in OpenAPI paths (`/openapi.json`, `/docs`, `/redoc`) win.  
3. Remaining **GET**s try `static_dir`.  
4. Otherwise **404**.

So `CREATE ROUTE home GET '/' AS SELECT … AS html` overrides a static `index.html` at `/`.

---

## What is not built yet

| Wanted | Status |
|--------|--------|
| `static_dir` root mount | **Built** |
| Custom URL prefix (`/assets` → dir) | Coming — see [coming soon](coming-soon.md) |
| Forced `Content-Disposition` download sugar | Coming |

---

## HTML UI pattern

Pair static assets with an `html` route for the shell page:

```sql
CREATE ROUTE home GET '/' AS
SELECT '<!doctype html><html><body><script src="/app.js"></script></body></html>' AS html;

SELECT * FROM quackapi_serve(8000, static_dir := './static');
```

Or generate HTML with a template extension (for example community `tera`) and still serve CSS/JS from `static_dir`.

---

## Next

- [OpenAPI](openapi.md)  
- [Functions reference](../reference/functions.md) — full `quackapi_serve` signature
