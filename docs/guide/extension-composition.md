# Extension composition — handlers are SQL

**The whole product idea:** `CREATE ROUTE` / `CREATE STREAM` handlers are ordinary SQL.
Whatever you can `LOAD` and call from a `SELECT`, a request can call.

No C++ rewrite per companion. No second process for “the PDF service.” Same DuckDB session,
same address space, same type system.

```text
browser  ──►  quackapi (SQL handlers)
                    │
                    ├── local tables / views
                    ├── curl_httpfs / httpfs  (read_text, read_json, …)
                    ├── http_client           (http_get, http_post, …)
                    ├── sitting_duck          (via quack_from_*)
                    ├── quack                 (quack_query / ATTACH)
                    └── pdf / tera / …        (community companions)
```

Full DDL and function tables stay in the [README](../../README.md) and
[function reference](../reference/functions.md). This page is **recipes only**.

---

## Recipe 1 — Outbound gateway

Join remote HTTP with local tables inside one route. Prefer DuckDB’s shared
HTTP stack so `quackapi_serve` batteries (`curl_httpfs` when available) speed
you up for free — see [curl_httpfs](../curl_httpfs.md).

### GET proxy via `read_text` / httpfs

Runnable shape in-tree: [`examples/proxy_curl_httpfs.sql`](../../examples/proxy_curl_httpfs.sql).

```sql
LOAD curl_httpfs;   -- optional; batteries also try this on serve
LOAD quackapi;

CREATE OR REPLACE ROUTE proxy GET '/proxy/:url' AS
SELECT
  $url AS requested_url,
  content AS body,
  length(content) AS bytes
FROM read_text($url);

CREATE OR REPLACE ROUTE http_util GET '/http_util' AS
SELECT quackapi_http_util_name() AS http_util;

SELECT * FROM quackapi_serve(18080);
```

```sh
# percent-encode the remote URL into :url
curl -sS "http://127.0.0.1:18080/http_util"
# [{"http_util":"MultiCurl"}]   # when curl_httpfs is active
```

There is **no** public SQL `quackapi_fetch` / `quackapi_post`. C++ `QuackapiHttpFetch`
is an internal HTTPUtil helper (OAuth/OIDC wave). Route authors use **SQL readers**
or a community HTTP extension.

### POST / partner call via community `http_client`

When you need explicit POST + headers (not just GET of a URL):

```sql
LOAD http_client;
LOAD quackapi;

CREATE OR REPLACE ROUTE proxy_score POST '/score' AS
SELECT http_post(
  'https://api.partner.com/v1/score',
  map {
    'Authorization': 'Bearer ' || getenv('PARTNER_TOKEN'),
    'Content-Type': 'application/json'
  },
  $body::JSON
) AS partner_response;
```

(Shape from the [handler bridge notes](../../bridges/from_x/docs/handler_bridge.md).
Confirm `http_post` arity against the `http_client` extension you load.)

Join local state the same way you always would:

```sql
CREATE ROUTE enrich GET '/orders/:id' AS
SELECT o.*, r.content AS remote_blob
FROM orders o
CROSS JOIN read_text(o.partner_url) r
WHERE o.id = $id::INTEGER;
```

---

## Recipe 2 — SSE live (honest transport)

Push rows as Server-Sent Events. Full guide: [CREATE STREAM](stream.md).

```sql
LOAD quackapi;

CREATE STREAM ticks GET '/ticks' AS
SELECT i AS id, 'tick' AS msg FROM range(3) t(i);

-- long-lived poll
CREATE OR REPLACE STREAM live GET '/live' WITH (interval='1s') AS
SELECT now() AS ts;

SELECT * FROM quackapi_serve(8000);
```

```sh
curl -N http://127.0.0.1:8000/ticks
# Content-Type: text/event-stream
# id: 0
# data: {"id":0,"msg":"tick"}
```

**Honest limits (do not plan around these):**

| Want | Reality |
|------|---------|
| Browser `EventSource` / SSE | **Yes** — `CREATE STREAM … GET` |
| WebSocket Upgrade | **No** — bundled httplib has no Upgrade API; `CREATE STREAM … WS` errors |
| Streams in `quackapi_routes()` | **No** — use `quackapi_streams()` (`transport` is always `'sse'` today) |
| `REQUIRE` auth on streams | Not in the current version |

A bus extension (e.g. community pub/sub) can feed the `SELECT` behind the stream
when LOADed; quackapi only owns the SSE wire format.

---

## Recipe 3 — `sitting_duck` / `quack_from_x`

Point at an existing app tree; get **route + model IR rows**. Native table functions
ship in quackapi and **auto-INSTALL/LOAD `sitting_duck` FROM community** on first use.

```sql
LOAD quackapi;

-- Routes: method, path, handler_name, file, …
SELECT method, path, handler_name
FROM quack_from_fastapi('bridges/from_x/fixtures/fastapi_mini')
ORDER BY method, path;
-- GET  /articles/{slug}  get_article
-- POST /login            login

-- Models → field IR (then BODY SCHEMA by hand or generator)
SELECT model_name, field_name, field_type, is_required
FROM quack_from_fastapi_models('bridges/from_x/fixtures/fastapi_mini');
```

Siblings: `quack_from_rails`, `quack_from_express`, `quack_from_gin` (+ `_models`).

**What is automatic vs not:**

| Automatic | Escape hatch |
|-----------|----------------|
| Decorators / DSL → route IR | Imperative handler bodies (never transpiled) |
| Pydantic / validates / tags → field IR | Rewrite body as SQL, macro, or [queue](queue.md) worker |
| Review IR → write `CREATE ROUTE` | Keep a thin external service and call it (recipe 1) |

More narrative, corpus numbers, and migration tips: [Coming from FastAPI](../from-fastapi.md).
Bridge layout and tests: [`bridges/from_x/`](../../bridges/from_x/README.md).

---

## Recipe 4 — Quack mesh

**Composition, not a second server inside quackapi.** When the community **`quack`**
extension is LOADed, a handler can fan out to another DuckDB process over the
quack remote protocol — same pattern as any other TVF.

```sql
LOAD quack;      -- separate extension; not bundled into quackapi
LOAD quackapi;

-- Illustrative shape — use the quack version you actually install.
-- Common forms in the wild:
--   quack_query('quack:host:9494', 'SELECT …', token => '…')
--   ATTACH 'quack:host:9494' AS edge (DISABLE_SSL true);

CREATE ROUTE edge_health GET '/mesh/health' AS
SELECT *
FROM quack_query(
  'quack:localhost:9494',
  'SELECT 1 AS ok',
  token := 'your-token'
);

CREATE ROUTE edge_table GET '/mesh/items' AS
SELECT *
FROM quack_query(
  'quack:localhost:9494',
  'SELECT * FROM items LIMIT 100',
  token := 'your-token'
);
```

Auth bridge scalars on the **quackapi** side for RPC-style checks (used by
quack-compatible setups; no extra process):

```sql
SELECT (quackapi_verify_auth('site', 'k-secret')).ok;
SELECT quackapi_authentication('sess', 'token', 'token');
SELECT quackapi_authorization('sess', 'SELECT 1');
```

quackapi still only listens with `quackapi_serve`. Mesh nodes are whatever quack
servers you already run.

---

## Recipe 5 — PDF companion (one process)

README showcase: Closure-style PDF review is **one DuckDB process** — routes +
community `pdf` + optional `tera` + tables. Function names come from the **`pdf`**
extension you load (not from quackapi).

```sql
INSTALL pdf FROM community;
LOAD pdf;
LOAD quackapi;

-- Typical community surfaces (names as used in product docs):
--   read_pdf_words, pdf_redact, …
-- Confirm against the pdf extension README for your DuckDB version.

CREATE ROUTE doc_words GET '/docs/:id/words' AS
SELECT *
FROM read_pdf_words(d.path)
JOIN documents d ON d.id = $id::INTEGER;

-- HTML page if tera is loaded:
-- LOAD tera;
-- CREATE ROUTE case_page GET '/cases/:id' AS
-- SELECT tera_render(template, ctx) AS html
-- FROM app_templates, … WHERE name = 'case.html';

SELECT * FROM quackapi_serve(8000, memory_limit := '4GB');  -- headroom for PDF work
```

Calling the “PDF service” is a function call in the same address space — not an RPC.

---

## Non-goals (explicit)

These are **out of scope** for quackapi composition stories and this guide:

| Non-goal | Why |
|----------|-----|
| GraphQL | Not a quackapi transport; stay REST + SQL |
| Asio / custom async server rewrite | Inbound remains httplib |
| Browser WebSocket Upgrade | Use SSE (`CREATE STREAM … GET`) |
| `WITH HISTORY` / time-travel as a product feature | Ordinary DuckDB time-travel if *you* enable it; not a quackapi API |

Also not invented here: public `quackapi_fetch` / `quackapi_post` SQL TVFs.

---

## Next

1. [Routes & params](routes-and-params.md) — muscle memory for `$name::TYPE`
2. [SSE streams](stream.md) — full `CREATE STREAM` grammar
3. [Outbound curl_httpfs](../curl_httpfs.md) — client selection + `/healthz`
4. [from-fastapi](../from-fastapi.md) — migration narrative
5. [Function reference](../reference/functions.md) — every `quackapi_*`
