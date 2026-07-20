# FastAPI → quackapi parity

**Headline:** the versioned HTTP conformance harness scores **89 / 89 (100%)** against FastAPI-shaped expectations.

| | |
|--|--|
| Harness | `test/conformance/` |
| Method | Real HTTP against `quackapi_serve()` (interactive FIFO session) |
| Re-run | `bash test/conformance/run.sh` then `python3 test/conformance/render_scorecard.py` |
| Historical write-up | [FASTAPI_PARITY.md](FASTAPI_PARITY.md) |

```
PASS 89   FAIL 0   N/A 0
classes: MATCH 88, STRONGER 1, BUG 0
```

| Group | Score |
|-------|------:|
| auth | 6/6 |
| content_types | 7/7 |
| methods | 9/9 |
| openapi | 3/3 |
| params | 26/26 |
| routing | 12/12 |
| status_codes | 5/5 |
| validation | 21/21 |

---

## Concept map

| FastAPI | quackapi |
|---------|----------|
| `@app.get("/x")` | `CREATE ROUTE r GET '/x' AS SELECT …` |
| `@app.post` / put / patch / delete | `POST` / `PUT` / `PATCH` / `DELETE` methods on `CREATE ROUTE` |
| Path `{id}: int` | `'/items/:id'` + `$id::INTEGER` |
| `Query(10, le=100)` | `PARAM limit INTEGER DEFAULT 10 LE 100` |
| `Header()` / `Cookie()` | `PARAM x HEADER` / `PARAM s COOKIE` |
| Pydantic body model | `$field` binds from JSON object; optional `BODY SCHEMA '…'` |
| `Form()` | `application/x-www-form-urlencoded` → same `$field` |
| `File()` / `UploadFile` | multipart `$file`, `$filename` / `$file_filename` |
| `status_code=201` | `STATUS 201` |
| `RedirectResponse` | `STATUS 307 AS SELECT '/new' AS location` |
| `Response.set_cookie` | column `set_cookie` |
| `HTMLResponse` / `PlainTextResponse` | column `html` / `text` |
| `Depends(api_key)` | `CREATE AUTH …` + `REQUIRE site` |
| `HTTPBearer` / JWT | `CREATE AUTH j AS JWT ( SECRET '…' )` |
| `APIRouter(prefix=…, dependencies=…)` | `CREATE GROUP v1 WITH (prefix=…, auth=…)` |
| `include_router` | `CREATE ROUTE … GROUP v1` |
| SQLAlchemy list scaffold | `CREATE API FOR TABLE t` |
| `BackgroundTasks` / Celery | `CREATE QUEUE` + `quackapi_enqueue` / `dequeue` / `ack` |
| `EventSourceResponse` / SSE | `CREATE STREAM … GET` |
| `StaticFiles` | `quackapi_serve(…, static_dir := '…')` |
| auto OpenAPI + `/docs` + `/redoc` | built-in `/openapi.json`, `/docs`, `/redoc` |
| `CORSMiddleware` | `cors_origins` / `SET quackapi_cors_origins` |
| `response_model` | `SELECT` column list (projection *is* the model) |
| 422 `RequestValidationError` | same `{detail:[{loc,msg,type}]}` shape |
| 405 + `Allow` | built-in |
| trailing slash redirect | 307 (Starlette-style) |
| WebSocket | **not built** — use SSE |
| OIDC / sessions / middleware hooks | not built — see [coming soon](guide/coming-soon.md) |

---

## Where quackapi is **stronger**

| Behavior | Why |
|----------|-----|
| **DB-typed JSON** | Bool / number / null follow DuckDB column types — no stringly JSON by default |
| **SELECT list = response model** | No dual Pydantic layer to keep in sync |
| **Set-based handlers** | Validation + projection in one prepared query |
| **Strict int bind** | Rejects `1.5`, `1e2` (Pydantic v2 parity; fail closed) |
| **int64 overflow → 422** | Fail closed instead of accepting arbitrary big ints then 404-ing in app code |
| **Column-name responses** | `html` / `text` / `location` / `set_cookie` without framework response classes |
| **Row access + masking policies** | First-class claims-keyed security — no FastAPI core primitive |
| **Durable queue in the same DB file** | Broker-less jobs without Redis for single-process workers |

---

## Intentional divergences (still harness PASS)

| Topic | quackapi | FastAPI | Why |
|-------|----------|---------|-----|
| Response envelope | Always JSON **array of rows** for multi-column handlers | Often a bare object for one dict | SQL result-set semantics |
| CORS default | Off until configured | Off until middleware | Same opt-in spirit |
| Query vs body `loc` | Missing params use `query` when no body model field path applies | Body models use `body` | SQL `$param` surface; JSON body still uses `loc=["body",…]` when fields come from the body |

---

## Outside the 89-case harness

Gaps vs the full FastAPI *ecosystem* (not counted against 100%):

- WebSocket Upgrade  
- OIDC / OAuth2 browser code flow  
- Signed sessions + CSRF  
- Middleware BEFORE/AFTER  
- Response gzip  
- Accept-based multi-format (CSV/Arrow)  
- In-process TestClient  

Details: [FEATURE_STATUS.md §2.5](FEATURE_STATUS.md).

---

## Next

- [Coming from FastAPI (`quack_from_X`)](from-fastapi.md)  
- [Guided tour](index.md)
