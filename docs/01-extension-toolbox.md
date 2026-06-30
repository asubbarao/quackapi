# The DuckDB extension toolbox for a web framework

> We evaluated ~28 community extensions against the **parts list** (`00-parts-list.md`). This is the composed
> stack: every framework capability mapped to the extension(s) that provide it, **whether it loads** on
> `duckdb v1.5.3 / osx_arm64`, the canonical pick, and the non-obvious compositions. The lesson: almost no
> extension is a "web framework," but they *compose* into one.

## Loadability (tested live)

| Loads ✓ | Fails here ✗ |
|---|---|
| quack, harbor, http_client, tera, netquack, json_schema, quickjs, crawler, webbed, gsheets, ducktinycc, http_stats, httpfs_timeout_retry, curl_httpfs, otlp, poached, quack_oauth, redis, scalarfs, web_search, web_archive | rawduck (404), rusty_quack (404), sitemap (init error) |

"Fails here" = no build for this exact version/platform, **not** broken — they may load on a newer binary.

## Capability → extension map

### 1. Listener / server  🚧 *the boundary*
| Extension | Loads | Role |
|---|---|---|
| `quack` | ✓ | your current listener (httpserver's predecessor). Fixed `/query`-style; **discards URL path** |
| `harbor` | ✓ | modern DuckDB-over-HTTP: `GET /` (UI), `POST /sql` (NDJSON/JSON), `POST /ddb/*`, + a custom **authorization-function** hook. Cleaner Tier-1 host & self-dispatch target than quack — but still not arbitrary-path→SQL |
| `ducktinycc` | ✓ | compile a C listener *in-process* — but compiled fns are scalar UDFs (no persistent background socket cleanly); hard |
| `rusty_quack` | ✗ | just a hello-world **Rust extension template** — the scaffold to *build your own* path-aware listener |
| `duckdb_urlpattern` | (not on community list / GitHub) | native URLPattern matching — pairs with whatever listener for standard route semantics |

**Verdict:** none exposes arbitrary request-path→SQL. Cross the boundary with the **18-line shim/proxy** (Tier-2) or skip it (Tier-1, path-as-argument). A real path-aware listener is a custom-extension project (Rust template / ducktinycc).

### 2. Outbound HTTP / self-dispatch engine
| Extension | Loads | Role |
|---|---|---|
| `http_client` → `http_post_form` | ✓ | **the self-dispatch engine** — POST rendered SQL to the listener. Canonical |
| `httpfs_timeout_retry` | ✓ | timeout + retry settings for outbound IO — resilience middleware. **Loaded by default** (`http_retries=3`, `httpfs_retries_file_operation=3`) |
| `curl_httpfs` | ✓ | **THE DEFAULT httpfs client — hardcoded.** connection pool + HTTP/2 multiplexing + async IO, so a multi-file `read_text/read_csv([N urls])` fetches concurrently. Read path only (self-dispatch POST still uses `http_client`) |
| `crawler` | ✓ | outbound fetch + `css_select`/`htmlpath`/`jq` — a handler data source |

#### HTTP CLIENT POLICY — curl_httpfs is the law (soldered, not a toggle)

For a DuckDB **server**, the httpfs client choice decides whether HTTP fan-out is concurrent or serial. We hardcode the curl client:

```sql
SET httpfs_client_implementation = 'curl';   -- vs the stock serial 'httplib'
```

**Measured** (30 README urls, one session, `read_text([...])`): `curl` **0.021 s** vs `httplib` **10.164 s** — **~480×**. httplib is serial GET; it makes any HTTP-fanout handler look broken.

This is wired in **two** places, both of which must be flipped to opt out (that double-wiring is the "you have to mean it" escape hatch):
1. `framework.sql` bootstrap — governs the CLI instance (gates, `/openapi.json`).
2. `serve_brain.sql` `worker_main` — the C accept-loop init run on **every** request-serving worker connection (the one that matters; handler SQL executes there).

To revert to the stock serial client, change `'curl'` → `'httplib'` in **both**. Anything that genuinely needs httplib (rare) you opt into deliberately, server-wide.

### 3. Routing (path match + capture)
- **Native SQL** (proven) — segment-split + match + capture. Canonical.
- `netquack` ✓ — `extract_path_segments()`, `extract_query_parameters()` (table funcs) — the cleanest URL/query parser for our input.
- `duckdb_urlpattern` — standard URLPattern semantics if you want them.

### 4. Validation (Pydantic)
- **Native** `try_cast` + CASE (proven) — aggregates **all** errors into FastAPI's `422 detail[]`. Canonical for the rich error list.
- `json_schema` ✓ — `json_schema_validate(schema, doc)` → `true` or **throws** with a reason (fails on first error). Best as the **single source of truth**: one JSON Schema drives **OpenAPI** *and* a boolean validation gate. Use both: schema for OpenAPI + gate, CASE for the detailed 422.

### 5. Templating & response rendering
- `tera` ✓ — `tera_render(tpl, json)` (Jinja-equivalent), confirmed. Handler SQL + HTML. Canonical.
- `webbed` ✓ — the **GOAT web ext**: `read_xml`/`read_html`, `xml_to_json`/`json_to_xml`, **`xml_valid`** (validate the HTML/XML we render before returning), `html_extract_*`. Build + validate any markup response; parse XML/form request bodies.
- `crawler` `jq()` ✓ — JSON querying/reshaping for bodies & responses.

### 6. Serialization
- **Native** `json_object` / `to_json` / `json_group_array`. Canonical.
- `webbed` `xml_to_json` / `json_to_xml` for XML APIs.

### 7. Handler logic escape hatch
- `quickjs` ✓ — `quickjs(code)` / `quickjs_eval(fn, args...)`. Run **arbitrary JS** for imperative handler logic SQL can't express. This is what lets a handler do "real code" like a Python view.

### 8. Handler-SQL security guard  *(outside-the-box)*
- `poached` ✓ — parses **SQL**: `is_valid_sql`, `parse_tables`. Run on the *rendered handler SQL before self-dispatch* to reject malformed/injection/over-reach (a handler may only touch whitelisted tables). A real safety layer.

### 9. Auth
- `quack_oauth` ✓ — full OAuth2/OIDC: `quack_oauth_check_token()` (validate incoming bearer via JWKS/introspection), `quack_oauth_check_authorization()` (SQL-native policy), `quack_oauth_audit_log()`. The real auth pillar. Pairs with quack/harbor.
- Simpler: listener Basic / `X-API-Key`, or a per-route check in `handle_request`.

### 10. Logging & observability  *(you said "logging is needed")*
- **Native** `CALL enable_logging(['QueryLog','HTTP'], storage => 'memory')` + the `duckdb_logs` view — HTTP + query logs, zero deps. (Your quack init already does this.)
- **A `request_log` table** the framework writes per request: `(ts, method, path, route_id, status, latency_ms, ...)` — the canonical inbound-API log, fully queryable.
- `otlp` ✓ — **read/analyze** OTel data (`read_otlp_traces/logs/metrics`); `otlp_serve()` ingests. Analysis side, not span emission.
- `http_stats` ✓ — request counts/bytes/time, but **outbound httpfs only**, surfaced via `EXPLAIN ANALYZE` (no status codes). Measures *our own* self-dispatch/outbound calls, not inbound API requests. Limited.
- *(rawduck `raw_ingest` would be an ideal schema-evolving log sink, but it doesn't load here.)*

### 11. State / cache / sessions
- `redis` ✓ — `redis_get/set/hget/hset/lpush/...` for session store, response cache, rate-limit counters. **No pub/sub** (so not a streaming bus). Experimental.

### 12. Static assets / self-data
- `scalarfs` ✓ — `to_scalarfs_uri` / `from_*_uri`: serve a SQL value as if it were a file. Static assets / the server's own data from SQL.

### 13. Handler data sources (demo "toy server" endpoints)
- `web_search` ✓ (`google_search`), `web_archive` ✓ (`wayback_machine`, `common_crawl_index`), `crawler` ✓, `gsheets` ✓, `netquack` ✓. Back real demo endpoints that proxy live data.

### 14. Hacks & novelty  *(edge-ledger gold)*
- `gsheets` ✓ — two hacks: **routes-in-a-Sheet** (non-devs edit the routes table in a spreadsheet) and **request/response-over-a-Sheet** (a Form append = inbound request; poll `read_gsheet`, respond via `COPY TO gsheet`) — a *listener-less* API where **Google hosts the socket**. Slow, hacky, zero-Python, sidesteps the boundary entirely.
- `ducktinycc` / Rust template — **build your own path-aware listener**. The ultimate flex.

## Canonical minimal stack (what v1 actually needs)

| Part | Pick |
|---|---|
| Listener | `quack` (or `harbor`) |
| Self-dispatch | `http_client` (`http_post_form`) |
| Routing | native SQL + `netquack` |
| Validation | native `try_cast`/CASE + `json_schema` (source of truth) |
| Templating | `tera` |
| Serialization | native `json_*` |
| Browser crossing | the 18-line shim |

**Strongly recommended:** `quack_oauth` (auth), native logging + a `request_log` table, `webbed` (HTML build/validate).

**Optional / showcase:** `quickjs` (JS handlers), `poached` (SQL guard), `redis` (cache/session), `scalarfs` (static), `crawler`/`web_search`/`web_archive`/`gsheets` (demo data + the gsheet hack), `httpfs_timeout_retry` (resilience), `otlp` (telemetry analysis), `harbor` (modern host), `duckdb_urlpattern` (standard matching).

## The one-line takeaway

The framework *brain* needs **zero** extensions. Three capabilities need one each (listener, outbound-POST, templating) and all three load. Everything else on this list is **optional power** — auth, JS handlers, SQL guards, caching, telemetry, demo data sources — that turns a toy into something that looks startlingly like a real framework. The only thing no extension gives you is *arbitrary-path→SQL*; that's the 18-line shim, or a custom listener you build.

Sources: [shreeve/duckdb-harbor](https://github.com/shreeve/duckdb-harbor), [teaguesterling/duckdb_urlpattern](https://github.com/teaguesterling/duckdb_urlpattern), and each extension's page at duckdb.org/community_extensions.
