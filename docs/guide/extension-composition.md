# Extension composition — handlers are SQL

**The whole product idea:** `CREATE ROUTE` / `CREATE STREAM` handlers are ordinary SQL.
Whatever you can `LOAD` and call from a `SELECT`, a request can call.

No C++ rewrite per companion. No second process for “the PDF service.” Same DuckDB session,
same address space, same type system.

```text
browser  ──►  quackapi (SQL handlers + thin GraphQL)
                    │
                    ├── local tables / views
                    ├── CREATE ROUTE / API FOR TABLE   (REST → your SQL)
                    ├── POST /graphql | GRAPHQL ROUTE  (GQL → SELECT …)
                    ├── curl_httpfs / httpfs  (read_text, read_json, …)
                    ├── httpfs_timeout_retry  (http_timeout / http_retries, per operation)
                    ├── otlp                  (otlp_serve — traces/metrics/logs as tables)
                    ├── cronjob               (cron — the queue drain's runner)
                    ├── radio / events        (event bus in; DB events out)
                    ├── http_client           (http_get, http_post, …)
                    ├── sitting_duck          (AST / quack_from_*; optional GQL parse recipe)
                    ├── quack                 (quack_query / ATTACH)
                    └── pdf / tera / …        (community companions)
```

**Dual surface:** GraphQL + ordinary HTTP on the same session — both resolve to SQL ([graphql-v0.md](graphql-v0.md)). Companions run *inside* handlers; they are not a second API product.

Full DDL and function tables stay in the [README](../../README.md) and
[function reference](../reference/functions.md). This page is **recipes only**.

---

## Recipe 1 — Outbound gateway

Join remote HTTP with local tables inside one route. Prefer DuckDB’s shared
HTTP stack so `quackapi_serve` can require the pooled `curl_httpfs` client — see
[curl_httpfs](../curl_httpfs.md).

### GET proxy via `read_text` / httpfs

Runnable shape in-tree: [`examples/proxy_curl_httpfs.sql`](../../examples/proxy_curl_httpfs.sql).

```sql
INSTALL curl_httpfs FROM community; -- one-time setup
LOAD curl_httpfs;
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

Point at an existing app tree; get **route + model IR rows**. The table functions ship
in quackapi; the extraction is `sitting_duck`'s. Install it once — quackapi `LOAD`s it
and refuses by name if it is missing, because a download inside a `SELECT` fails the
query on an offline box instead of the setup.

```sql
INSTALL sitting_duck FROM community;   -- one-time setup
```

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

## Recipe 6 — Observability is `otlp`, not a quackapi table

quackapi keeps **no request history of its own** — no ring buffer, no
`quackapi_requests()`. The community **`otlp`** extension runs a real OTLP/HTTP
receiver inside the same process and lands spans, metrics and logs as tables.

```sql
INSTALL otlp FROM community;   -- one-time setup
LOAD otlp;
LOAD quackapi;                 -- default-creates otlp:localhost:4318 and says so

SELECT uri, catalog, state, detail FROM quackapi_otlp();
-- otlp:localhost:4318  (empty)  serving  otlp_serve

SELECT * FROM read_otlp_traces();
```

| Knob | Effect |
|------|--------|
| `SET quackapi_otlp = 'local'` | default — loopback receiver when `otlp` is loaded |
| `SET quackapi_otlp = 'off'` | create nothing |
| `SET quackapi_otlp = 'otlp:0.0.0.0:4318'` | explicit endpoint; **`quackapi_serve` fails** if `otlp` is missing |
| `SET quackapi_otlp_catalog = 'my_ducklake'` | durable ingest into a DuckLake or Iceberg catalog instead of local tables |

The default binds loopback on purpose. **Beyond this box, an OpenTelemetry
Collector is the answer** — point it at the endpoint, or at your backend, and let
it do the fan-out, batching and retention a database should not.

`quackapi_serve` never downloads `otlp`. With `quackapi_otlp` set to an explicit
URI and the extension missing, it refuses to bind and names it.

---

## Recipe 7 — Queue workers run on `cronjob`

The queue's drain is SQL. The runner is the community **`cronjob`** extension, in
this process:

```sql
INSTALL cronjob FROM community;   -- one-time setup
LOAD cronjob;

CREATE QUEUE emails;
SELECT * FROM quackapi_queue_worker('emails');            -- every second
SELECT * FROM quackapi_queue_worker('emails', schedule := '*/5 * * * * *',
                                    sql := 'SELECT send(payload) FROM quackapi_dequeue(''emails'', 10)');
```

`cron_jobs()` and `cron_delete(job_id)` stay `cronjob`'s — quackapi wraps
neither. A process that is already obliged to stay online is exactly what an
external scheduler exists to work around, so the scheduler moves in.

Full surface: [queue guide](queue.md).

---

## Recipe 8 — Event bus in, database events out

quackapi owns the SSE wire format and nothing else about messaging.

- **`radio`** (`query-farm/radio`) is the event-bus client: `radio_subscribe`,
  `radio_received_messages()`, `radio_transmit_message` against WebSocket or
  Redis pub/sub. A `CREATE STREAM` whose `SELECT` reads
  `radio_subscription_received_messages(...)` is a bus-fed SSE endpoint with no
  quackapi code involved.
- **`events`** (`query-farm/events`) turns DuckDB's own lifecycle — connections,
  queries, transactions — into JSON on an external program's stdin, via
  `events_destination` / `events_types`. It needs nothing from quackapi either.

Neither has a quackapi wrapper, on purpose: a wrapper would be a second API over
somebody else's, and the first thing to rot.

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
