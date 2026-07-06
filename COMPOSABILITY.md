# The Composability Receipts

**Claim:** handlers are SQL, so any `LOAD`ed DuckDB extension composes inside a request with
**zero framework changes**. FastAPI's composition layer is pip + glue code per feature; here each
feature is `LOAD x` plus an `INSERT INTO routes`.

**Proof:** `compose.sql` registers six routes, each powered by a different community extension.
No line of `framework.sql` was modified. Every output below is real — regenerate with:

```bash
bash test/compose_receipts.sh
```

## R1 — `json_schema`: declarative document validation (Pydantic-class, one function)

`POST /check/{schema_id}` validates a document against a JSON Schema stored as data
(`compose_schemas`). Full draft semantics — `required`, `minLength`, integer ranges,
`additionalProperties: false` — none of it hand-rolled.

```
POST /check/order  {"doc": "{\"sku\":\"ABC123\",\"qty\":5}"}
  -> 200 {"schema_id":"order","parseable":true,"valid":true}
POST /check/order  {"doc": "{\"sku\":\"ABC123\",\"qty\":5000,\"hack\":1}"}   (qty > max, extra prop)
  -> 200 {"schema_id":"order","parseable":true,"valid":false}
POST /check/order  {"doc": "not json at all"}
  -> 200 {"schema_id":"order","parseable":false,"valid":false}
```

Note: `json_schema_validate` **throws** on an invalid instance (with an excellent message);
the handler wraps it in `try(...)` + `coalesce(..., false)` for a boolean API.

## R2 — `finetype`: semantic type inference as an endpoint (244 semantic types)

```
GET /classify?value=192.168.1.1
  -> 200 {"value":"192.168.1.1","semantic_type":"technology.internet.ip_v4",
          "detail":{"type":"technology.internet.ip_v4","confidence":0.901,"duckdb_type":"INET",...}}
GET /classify?value=alok@example.com
  -> 200 {"value":"alok@example.com","semantic_type":"identity.person.email",...}
```

## R3 — `crypto`: HMAC webhook signing in the request pipeline

Server-side secret lives in a table; the handler signs the payload inline.

```
POST /webhooks/sign  {"payload": "order:12345:shipped"}
  -> 200 {"algo":"hmac-sha2-256",
          "signature":"91b443efefc6a040e2d701dfaf90263a4bbeac4226fcb8820fe283ce8bc0352f"}
```

## R4 — `tera`: server-rendered HTML over live table data (the Jinja2 slot)

Template stored as data; rendered in-engine next to the rows it reports on.

```
GET /report
  -> 200 <html><body><h1>Users (3)</h1><ul><li>alice (30)</li><li>bob (25)</li><li>carol (40)</li></ul></body></html>
```

## R5 — `parser_tools`: a SQL-linting endpoint (the engine's own parser as a service)

```
POST /sql/lint  {"q": "SELECT 1 FROM t WHERE x > 2"}   -> 200 {"parseable":true}
POST /sql/lint  {"q": "SELEKT oops FROM"}              -> 200 {"parseable":false}
```

## R6 — `curl_httpfs`: parallel HTTP fan-out inside one request

Three upstream URLs fetched concurrently (pooled connections, HTTP/2) by one table function.
FastAPI's equivalent is an async client + `asyncio.gather` + response plumbing.

```
GET /fanout
  -> 200 [{"url":"https://raw.githubusercontent.com/duckdb/duckdb/main/README.md","bytes":3480},
          {"url":"https://raw.githubusercontent.com/duckdb/community-extensions/main/README.md","bytes":492},
          {"url":"https://raw.githubusercontent.com/duckdb/duckdb-web/main/README.md","bytes":930}]
```

## And the validation pipeline still guards every receipt

```
POST /webhooks/sign  {}
  -> 422 {"detail":[{"type":"missing","loc":["body","payload"],"msg":"Field required"}]}
```

## Gaps found while building the receipts (honest ledger)

1. **No dynamic-HTML kind.** `kind='html'` is hardwired to the Swagger UI body in
   `rendered_static`; a dynamic handler cannot declare `text/html`. R4 ships as `kind='dynamic'`
   (correct body, `application/json` content type) until a `dynamic_html` kind (or
   route_headers CT override) exists.
2. **No percent-decoding of query values.** `?value=alok%40example.com` reaches the handler
   still encoded (FastAPI decodes). Real conformance divergence; belongs in the conformance
   case list.
3. **`register_route` output columns are unnamed** (only `route_id` is aliased), so
   `INSERT ... BY NAME` can't bind — registrations must be positional. Alias every column in
   the macro (`method AS method`, ...) to make route registration BY NAME-safe.
4. **`dqtest` is not published for osx_arm64** — dropped from the receipt set; validation
   receipts stand on `json_schema` + `finetype`.

---

# Wave 2 — six more extensions, same rules

Regenerate: `bash test/compose_receipts.sh` (R7–R12) and `bash test/compose_cron_fire.sh` (firing proof).

## R7 — `fts` (core, 5.2 MB): BM25 full-text search endpoint

FastAPI's answer to "add search" is running Elasticsearch. Here it's one PRAGMA at boot.

```
GET /articles/search?q=pond
  -> 200 [{"id":4,"title":"Winter pond maintenance","score":0.396},
          {"id":2,"title":"Feeding your flock","score":0.308}]
GET /articles/search?q=database
  -> 200 [{"id":1,"title":"Ducks and databases","score":0.42},
          {"id":3,"title":"HTTP servers in strange places","score":0.308}]
```

## R8 — `cronjob` (24 MB): background jobs, the Celery slot — WITH FIRING PROOF

```
POST /jobs/heartbeat  {"schedule": "*/10 * * * * *"}   -> 201 {"scheduled":"task_1"}
GET  /jobs                                             -> live job list via cron_jobs()
```

`test/compose_cron_fire.sh` holds one session open 35s after scheduling:

```
~~FIRED~~3 heartbeats: 15:20:30, 15:20:40, 15:20:50
```

Three executions at exact 10-second intervals AFTER the scheduling call returned —
**fire-and-forget background execution inside the engine.** This defeats edges.md
hypothesis #4 by extension. (Cron expressions are 6-field, seconds-first.)

## R9 — `bitfilters` (6.8 MB): probabilistic membership (xor filter)

The primitive under rate limiting / dedup / seen-this-key. Filter built by an aggregate
over a live table, probed per request via core `hash()`.

```
GET /allowlist/check?id=key-bravo   -> 200 {"id":"key-bravo","known":true}
GET /allowlist/check?id=intruder    -> 200 {"id":"intruder","known":false}
```

## R10 — `rapidfuzz` (4.9 MB): typo-tolerant fuzzy lookup

```
GET /users/fuzzy?name=alicce   -> 200 {"best_match":"alice","score":90.9}
```

## R11 — `markdown` (5.4 MB): md → HTML rendering in-engine

```
POST /render/md  {"md": "A **bold** claim with a [link](https://duckdb.org) and *italics*"}
  -> 200 {"html":"<p>A <strong>bold</strong> claim with a <a href=\"https://duckdb.org\">link</a> and <em>italics</em></p>\n"}
```

## R12 — `postgres` (29.4 MB): the PostgREST claim (`compose_pg.sql`)

ATTACH a **live Postgres** and serve REST over its tables in two route inserts — with the
framework's validation still guarding the path params. PostgREST is an entire product;
this is `LOAD postgres` plus route data.

```
GET /pg/products
  -> 200 [{"id":1,"name":"duck feed","price":9.99,"in_stock":true},
          {"id":2,"name":"pond liner","price":149.5,"in_stock":true},
          {"id":3,"name":"decoy","price":24.0,"in_stock":false}]
GET /pg/products/2     -> 200 {"id":2,"name":"pond liner","price":149.5,"in_stock":true}
GET /pg/products/abc   -> 422 {"detail":[{"type":"int_parsing","loc":["path","id"],
                                "msg":"Input should be a valid integer, unable to parse
                                 string as an integer","input":"abc"}]}
```

Setup (once): `createdb quackapi_demo` + a `products` table. `compose_pg.sql` is a separate
file so machines without Postgres lose one receipt, not the framework.

## Wave-2 notes for the ledger

- The R12 422 body carries the `input` field — the richer-422 work and these receipts
  compose without knowing about each other, which is itself the thesis.
- **Dropped by design: `mlpack` (24.6 MB) and `vss` (24.3 MB).** Both are real, but adopting
  them makes this an *ML server* — a different product with a different weight class. mlpack's
  table-name-argument interface needs its own handler pattern, and vss needs an external
  embedding producer. The thesis here is "FastAPI's composition layer at FastAPI's weight";
  staying in that weight class is the feature. Anyone who wants them can `LOAD` them — that's
  the point — but they are not part of the framework's claimed surface.
- Deferred (not dropped): `sshfs`.
- Total added weight for wave 2: ~95 MB across six extensions, five of them under 25 MB.
  For scale: a `pip install fastapi uvicorn` venv is ~35–40 MB before you add a single
  feature dependency (ES client, celery, redis, httpx, jinja2...). The receipt set above
  covers those feature slots inside the same ~95 MB.
