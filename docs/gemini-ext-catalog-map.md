# Gemini brainstorm × `ext_catalog_clean` — composition map

**Source:** live quack `:9494` `ext_catalog_clean` (2026-07-29).  
**Method:** `ILIKE` on `extension_name` / `content`; samples only from `query IS NOT NULL` or README blocks — no invented signatures.  
**Thesis:** compose first. Prefer a **recipe** (LOAD + `CREATE ROUTE` / `CREATE STREAM` / `CREATE QUEUE`) over new C++. Thin C++ only when the wire format or registry lives inside quackapi.

**Already in quackapi (code / DDL, not just SPECs):**

| Surface | Status in tree |
|---------|----------------|
| `CREATE ROUTE … RATE LIMIT n PER s [BY ip\|token\|key]` → 429 | Built (`quackapi_ddl` / server) |
| `FORMAT json\|ndjson\|csv` + Accept negotiation | Built |
| Response `gzip` / `zstd` via `Accept-Encoding` | Built in `quackapi_server.cpp` (`compression` serve knobs) — **FEATURE_STATUS.md is stale** on gzip |
| `CREATE STREAM … GET` SSE | Built; **WS rejected** (httplib) |
| `CREATE QUEUE` + workers | Built |
| `CREATE AUTH` JWT / API_KEY | Built; **OIDC browser code-flow not** |
| Batteries: try `curl_httpfs`, auth verify helpers, `quack_from_*` | Built |
| Composition recipes | [extension-composition.md](guide/extension-composition.md) |

---

## Theme map (14)

Status vocabulary:

| Status | Meaning |
|--------|---------|
| **EXISTS_COMPOSE** | Extension(s) implement the capability; ship as recipe |
| **EXISTS_PARTIAL** | Related surface exists; gap remains (wrong layer, client-only, incomplete) |
| **NOT_IN_CATALOG** | No extension delivers this for quackapi’s HTTP plane |
| **ALREADY_IN_QUACKAPI** | Product already owns it |

---

### 1. GraphQL (auto schema, HTTP GQL server, SQL→GQL)

| Field | Value |
|-------|--------|
| **Status** | **NOT_IN_CATALOG** |
| **extension_name(s)** | incidental only: `sitting_duck` lists GraphQL as a **parse language** (AST), not a GraphQL server |
| **Compose with ROUTE/STREAM** | Thin product entrypoint: `POST /graphql` handler that maps fields → tables/`quackapi_routes()` SQL; no extension replaces the HTTP GraphQL protocol |
| **Sample (catalog)** | No GraphQL server sample. sitting_duck language matrix only: languages include `SQL, DuckDB, GraphQL, JSON` |
| **Ship as** | **Recipe for IR** (schema dump from `information_schema` / `quackapi_routes()`) + **thin C++ or pure-SQL router v0** if you want a product surface. Do **not** invent a full GQL engine in C++ first. |

---

### 2. WebSocket / realtime / pubsub / SSE bus

| Field | Value |
|-------|--------|
| **Status** | **EXISTS_PARTIAL** (+ **ALREADY_IN_QUACKAPI** for SSE wire) |
| **extension_name(s)** | **`radio`** (Query.Farm: WebSocket **clients**, message queues, event buses — listen + broadcast into queryable buffers). SSE owned by quackapi. |
| **Compose** | `CREATE STREAM … GET` for browser EventSource; feed stream SQL from tables/views that **radio** fills. No `CREATE STREAM … WS` on httplib. |
| **Sample (catalog)** | ```sql<br>INSTALL radio FROM community;<br>LOAD radio;<br>``` README: “interact seamlessly with real-time event systems such as WebSocket servers, message queues, and event buses” — catalog has almost no function samples (README thin). Confirm live symbols after LOAD. |
| **Ship as** | **Recipe** (`radio` in + SSE out). **Not** browser WebSocket Upgrade on quackapi port (SKIP-BLOAT / transport). |

---

### 3. Rate limit / quota (beyond route RATE LIMIT)

| Field | Value |
|-------|--------|
| **Status** | **ALREADY_IN_QUACKAPI** (HTTP 429) + **EXISTS_COMPOSE** (I/O quota) |
| **extension_name(s)** | **`rate_limit_fs`** — GCRA bandwidth/ops limits on wrapped filesystems (local/httpfs), orthogonal to per-route HTTP limits |
| **Compose** | Route: `CREATE ROUTE … RATE LIMIT 100 PER 60 BY ip`. Handler I/O: wrap FS before heavy `read_*` / `COPY`. |
| **Sample (catalog)** | ```sql<br>INSTALL rate_limit_fs FROM community;<br>LOAD rate_limit_fs;<br>SELECT rate_limit_fs_wrap('LocalFileSystem');<br>SELECT rate_limit_fs_quota('RateLimitFileSystem - LocalFileSystem', 'read', 1048576, 'blocking');<br>SELECT rate_limit_fs_burst('RateLimitFileSystem - LocalFileSystem', 'read', 10485760);<br>SELECT * FROM rate_limit_fs_configs();<br>``` |
| **Ship as** | **Recipe / docs** only for dual-layer (HTTP vs FS). No second rate-limit C++. |

---

### 4. Response cache / ETag / CDN

| Field | Value |
|-------|--------|
| **Status** | **EXISTS_PARTIAL** / **NOT_IN_CATALOG** for HTTP ETag-304 |
| **extension_name(s)** | **`cache_httpfs`** — *remote read* FS cache above httpfs (not HTTP response Cache-Control/ETag). **`query_condition_cache`** — predicate/result acceleration inside DuckDB, not CDN. **`cache_prewarm`** — unrelated warm path. |
| **Compose** | Outbound: `LOAD cache_httpfs` then `read_*` remote URLs inside routes. Inbound 304/ETag: still product thin-glue (`CACHE TTL` + hash table) — **no catalog replacement**. |
| **Sample (catalog)** | ```sql<br>INSTALL cache_httpfs FROM community;<br>LOAD cache_httpfs;<br>``` (README: “read-only filesystem for remote access … cache layer above duckdb httpfs”) |
| **Ship as** | **Recipe** for outbound FS cache. **Thin C++** only if you want declarative `CACHE TTL` / ETag-304 on responses. |

---

### 5. Arrow IPC / Flight / ADBC / columnar HTTP

| Field | Value |
|-------|--------|
| **Status** | **EXISTS_COMPOSE** (codec + **clients**) |
| **extension_name(s)** | **`nanoarrow`** (alias **`arrow`**) — read/write Arrow IPC streams/files; `to_arrow_ipc` BLOB buffers. **`adbc`** / **`adbc_scanner`** — ADBC drivers/scanners. **`airport`** — Arrow **Flight client** (“query, modify, and store data via Arrow Flight servers”). |
| **Compose** | Handler `SELECT * FROM to_arrow_ipc((FROM my_q))` once FORMAT ARROW exists; or write `.arrows` and stream bytes. Flight **server** stays external — airport/adbc attach **to** Flight, do not embed one in quackapi. |
| **Sample (catalog)** | ```sql<br>INSTALL nanoarrow FROM community;<br>LOAD nanoarrow;<br>LOAD httpfs;<br>SELECT commit, message<br>FROM 'https://github.com/apache/arrow-experiments/raw/refs/heads/main/data/arrow-commits/arrow-commits.arrows';<br><br>COPY (SELECT 42 AS foofy, 'string' AS stringy) TO 'test_2.arrows' (FORMAT ARROWS);<br>-- buffers: FROM to_arrow_ipc((FROM T))  → blob + header flag columns<br><br>INSTALL airport FROM community;<br>LOAD airport;<br>INSTALL adbc FROM 'http://duckdb.columnar.tech';<br>LOAD adbc;<br>``` |
| **Ship as** | **Recipe** for clients + **thin FORMAT arrow** (serdes) after parquet. **Not** Flight server C++. |

---

### 6. Parquet export over HTTP (write path)

| Field | Value |
|-------|--------|
| **Status** | **NOT_IN_CATALOG** as HTTP body codec |
| **extension_name(s)** | Core parquet (read/write files). **`delta_export`** — Delta export, not HTTP. No “parquet response” extension. |
| **Compose** | `COPY (SELECT …) TO '/tmp/x.parquet'` then static/file route — or product **`FORMAT parquet`** serializing handler rows to `application/vnd.apache.parquet` / `PAR1` bytes. |
| **Sample** | Core: `COPY (SELECT * FROM t) TO 'out.parquet' (FORMAT PARQUET);` — not catalog-specific |
| **Ship as** | **Thin C++ FORMAT parquet** (earned). Recipe-only file-then-static is OK for demos. |

---

### 7. OAuth / OIDC / sessions / JWT extras

| Field | Value |
|-------|--------|
| **Status** | **EXISTS_PARTIAL** |
| **extension_name(s)** | **`quack_oauth`** (153 blocks) — OAuth 2.1 / OIDC for **duckdb-quack** wire (JWKS, RFC 7662 introspect, client_credentials, device_code, SQL policy table). **`jwt`** — decode payload / claims. quackapi already: JWT + API_KEY `CREATE AUTH`. |
| **Compose** | Token verify routes with `jwt_decode_payload`; long-term glue: OIDC secrets/policy via quack_oauth patterns **or** handler SQL that POSTs to IdP with `http_client` / curl_httpfs. Browser SSO code-flow still needs quackapi callbacks. |
| **Sample (catalog)** | ```sql<br>-- jwt<br>INSTALL jwt; LOAD jwt;<br>SELECT jwt_decode_payload('eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9…') AS payload;<br><br>-- quack_oauth resource-server secret (generic IdP)<br>CREATE SECRET rs (<br>  TYPE quack_oauth_server,<br>  issuer 'https://idp.example.com/',<br>  jwks_uri 'https://idp.example.com/.well-known/jwks.json',<br>  introspection_endpoint 'https://idp.example.com/oauth2/introspect',<br>  introspect_client_id 'my-rs', introspect_client_secret 'shh',<br>  audience 'my-quack-api'<br>);<br>SET quack_oauth_provider = 'generic';<br>SET quack_oauth_server_secret_name = 'rs';<br><br>INSERT INTO main.policies VALUES<br>  (10, NULL, ['analyst'], ['Attach', 'Scan'], NULL, NULL, true);<br>``` |
| **Ship as** | **Recipe** for jwt claims in handlers + mesh auth. **Thin C++** only for `CREATE AUTH … OIDC` browser flow if product wants it (PARTIAL-EXT). |

---

### 8. Middleware / hooks / before-after

| Field | Value |
|-------|--------|
| **Status** | **NOT_IN_CATALOG** |
| **extension_name(s)** | none (no `middleware` / request-hook extension) |
| **Compose** | Today: put shared SQL in views/macros (user-approved) or duplicate preamble in handlers; or product `CREATE MIDDLEWARE … BEFORE\|AFTER` |
| **Sample** | — |
| **Ship as** | **Thin C++ registry** if declarative BEFORE/AFTER is the goal; otherwise **SQL views** as recipe (no new extension). |

---

### 9. Distributed mesh / multi-node / quack protocol

| Field | Value |
|-------|--------|
| **Status** | **EXISTS_COMPOSE** (+ recipe already in composition guide) |
| **extension_name(s)** | **`quack`** — remote protocol `quack_serve` / `quack_query` / `ATTACH`. **`sazgar`** — query routing to remote DBs under CPU/RAM pressure + SQL dialect translate. **`airport`** — Flight mesh. **`quackscale`**, **`ducksync`** — scale/cache companions (different products). **`httpserver`** — sibling OLAP HTTP server (compete/complement, not compose inside quackapi). |
| **Compose** | `CREATE ROUTE … AS SELECT * FROM quack_query('quack:host:9494', '…', token => '…')`. |
| **Sample (catalog)** | ```sql<br>INSTALL quack FROM core; LOAD quack;<br>CALL quack_serve('quack:0.0.0.0:9494', token => 'vori-quack-2026', allow_other_hostname => true);<br>FROM quack_query('quack:hillside:9494', 'SELECT * FROM store_products', token => 'vori-quack-2026');<br><br>-- sazgar<br>SELECT * FROM sazgar_target('postgres_prod', 'host=… port=5432 …');<br>SELECT * FROM sazgar_route('SELECT * FROM sales LIMIT 100', 'postgres_prod', 'TRUE', '');<br>``` |
| **Ship as** | **Recipe only**. Do not reimplement quack protocol inside quackapi. |

---

### 10. Zero-copy / high-perf HTTP (uwebsockets, asio)

| Field | Value |
|-------|--------|
| **Status** | **NOT_IN_CATALOG** |
| **extension_name(s)** | incidental “zero-copy” wording in data codecs (`rawduck`, adbc notes) — **not** an HTTP server stack. No uWebSockets/asio HTTP host extension. |
| **Compose** | Stay on httplib + optional curl outbound; high-TPS custom host = product rewrite, not catalog. |
| **Sample** | — |
| **Ship as** | **Do not build** from Gemini “replace httplib with uWS” unless measured need. Prefer SSE + quack mesh. |

---

### 11. Code → routes (sitting_duck, parser_tools, sazgar)

| Field | Value |
|-------|--------|
| **Status** | **EXISTS_PARTIAL** / **ALREADY_IN_QUACKAPI** (`quack_from_*`) |
| **extension_name(s)** | **`sitting_duck`** — `read_ast` multi-language (used by `quack_from_fastapi` etc.). **`parser_tools`** — `parse_tables` / `parse_functions` / `is_parsable` on SQL. **`sazgar`** — dialect translate + remote route (not app-framework IR). **`urlpattern`** — URLPattern matching/build for path IR. |
| **Compose** | `SELECT * FROM quack_from_fastapi('path')` → hand-write / generate `CREATE ROUTE`. parser_tools validates handler SQL. urlpattern for pattern tests outside quackapi’s path matcher. |
| **Sample (catalog)** | ```sql<br>LOAD sitting_duck;<br>SELECT name, semantic_type AS signature, start_line<br>FROM read_ast('my_script.py', 'python', context := 'native')<br>WHERE is_function_definition(semantic_type);<br><br>INSTALL parser_tools FROM community; LOAD parser_tools;<br>SELECT * FROM parse_tables('SELECT * FROM MyTable');<br>SELECT * FROM parse_functions('SELECT count(*) FROM t');<br><br>SELECT urlpattern_test('/users/:id', 'https://example.com/users/456');<br>SELECT urlpattern_extract('/users/:id', 'https://example.com/users/456', 'id');<br>``` |
| **Ship as** | **Recipe + bridge polish**. No second AST stack in C++. |

---

### 12. Outbound HTTP quality (curl_httpfs, http_client, netquack)

| Field | Value |
|-------|--------|
| **Status** | **EXISTS_COMPOSE** / **ALREADY_IN_QUACKAPI** (curl batteries) |
| **extension_name(s)** | **`curl_httpfs`** — MultiCurl HTTPUtil for `read_text`/`read_json`/httpfs. **`http_client`** — experimental `http_get` / `http_post` (+ form). **`netquack`** — **URI/domain/IP parsing** (not an HTTP client). **`httpfs_timeout_retry`**, **`http_stats`**, **`crawler`**, **`cache_httpfs`** — quality/ops around HTTP I/O. |
| **Compose** | GET proxy: `FROM read_text($url)` with curl batteries. Explicit POST: `http_client`. URL analytics: `netquack`. |
| **Sample (catalog)** | ```sql<br>INSTALL http_client FROM community; LOAD http_client;<br>-- Functions documented under GET / POST / form (arity: confirm after LOAD)<br><br>INSTALL netquack FROM community; LOAD netquack;<br>SELECT * FROM ipcalc('192.168.1.0/24');<br><br>-- curl_httpfs: INSTALL/LOAD; product documents MultiCurl via quackapi_http_util_name()<br>``` |
| **Ship as** | **Recipe** (already Recipe 1). Harden fail-loud when `http_client:='curl'` forced but missing — small product quality, not new stack. |

---

### 13. Compression gzip/zstd for responses

| Field | Value |
|-------|--------|
| **Status** | **ALREADY_IN_QUACKAPI** |
| **extension_name(s)** | none required — miniz/zstd in DuckDB; quackapi negotiates `Accept-Encoding` (prefer zstd, then gzip) |
| **Compose** | `quackapi_serve(..., compression := true, …)` — document knobs; no community ext |
| **Sample** | (product code, not catalog) |
| **Ship as** | **Docs / FEATURE_STATUS fix**. Catalog has no gzip HTTP extension because core already covers it. |

---

### 14. Background jobs / cron beyond CREATE QUEUE

| Field | Value |
|-------|--------|
| **Status** | **EXISTS_COMPOSE** + **ALREADY_IN_QUACKAPI** (QUEUE) |
| **extension_name(s)** | **`cronjob`** — schedule SQL with cron expressions while process is up; `cron_jobs()` / `cron_delete` |
| **Compose** | `cron('INSERT INTO jobs …', '0 */15 * * * *')` to enqueue; workers via `CREATE QUEUE`. Or cron runs maintenance SQL directly. |
| **Sample (catalog)** | ```sql<br>INSTALL cronjob FROM community; LOAD cronjob;<br>SELECT cron('SELECT now()', '*/15 * 1-4 * * *');<br>SELECT cron('SELECT cleanup()', '0 0 7 ? * MON-FRI');<br>SELECT * FROM cron_jobs();  -- catalog describes columns<br>SELECT cron_delete('task_0');<br>``` |
| **Ship as** | **Recipe** pairing cronjob + QUEUE. No second scheduler in C++. |

---

## Summary matrix

| # | Theme | Status | Prefer |
|---|--------|--------|--------|
| 1 | GraphQL | NOT_IN_CATALOG | thin v0 + recipe; not full engine |
| 2 | WS / bus / SSE | EXISTS_PARTIAL | radio recipe + SSE; no browser WS |
| 3 | Rate / quota | ALREADY + EXISTS_COMPOSE | docs dual-layer + rate_limit_fs |
| 4 | Cache / ETag | EXISTS_PARTIAL | cache_httpfs outbound; ETag = thin C++ |
| 5 | Arrow / Flight / ADBC | EXISTS_COMPOSE | nanoarrow FORMAT; no Flight server |
| 6 | Parquet HTTP write | NOT_IN_CATALOG | thin FORMAT parquet |
| 7 | OAuth / OIDC / JWT | EXISTS_PARTIAL | jwt + quack_oauth recipes; OIDC SSO thin |
| 8 | Middleware | NOT_IN_CATALOG | views or thin CREATE MIDDLEWARE |
| 9 | Mesh / multi-node | EXISTS_COMPOSE | quack / sazgar / airport recipes |
| 10 | Zero-copy HTTP host | NOT_IN_CATALOG | do not build |
| 11 | Code → routes | EXISTS_PARTIAL / ALREADY | sitting_duck bridges + parser_tools |
| 12 | Outbound HTTP | EXISTS_COMPOSE / ALREADY | curl_httpfs + http_client recipes |
| 13 | gzip/zstd | ALREADY_IN_QUACKAPI | document; fix FEATURE_STATUS |
| 14 | Cron / jobs | EXISTS_COMPOSE / ALREADY | cronjob + QUEUE recipe |

---

## Top 15 extensions that scream “compose with quackapi”

**Not already covered as first-class recipes** in [docs/guide/extension-composition.md](guide/extension-composition.md)  
(that page already has: `curl_httpfs`, `http_client`, `sitting_duck`/`quack_from_*`, `quack`, `pdf`/`tera`, SSE).

| # | extension | Why it pairs with CREATE ROUTE / STREAM |
|---|-----------|----------------------------------------|
| 1 | **`radio`** | Bus → tables → SSE streams |
| 2 | **`rate_limit_fs`** | Protect handler I/O / remote FS beside HTTP RATE LIMIT |
| 3 | **`cache_httpfs`** | Outbound remote-read cache for proxy routes |
| 4 | **`nanoarrow`** | Columnar response bodies (`to_arrow_ipc` / ARROWS) |
| 5 | **`airport`** | Handler as Flight **client** to external columnar services |
| 6 | **`adbc` / `adbc_scanner`** | Heterogeneous DB fan-in inside one route |
| 7 | **`quack_oauth`** | Real OIDC/JWKS/policy for mesh-adjacent auth |
| 8 | **`jwt`** | Claim extract in SQL handlers / auth enrichment |
| 9 | **`cronjob`** | Time-driven enqueue + maintenance next to QUEUE |
| 10 | **`parser_tools`** | Validate/analyze handler SQL; schema tooling |
| 11 | **`sazgar`** | Spill heavy SELECT to remote when local load high |
| 12 | **`urlpattern`** | Path/query IR, tests, URL build in migration tools |
| 13 | **`netquack`** | Host/TLD/IP analytics on request or log tables |
| 14 | **`duckdb_mcp`** | Same DuckDB process: MCP for agents **and** quackapi for HTTP |
| 15 | **`ai`** | `ai_summarize` / complete inside a route (gateway shape) |

Honorable mentions (also not in composition.md): `shellfs`, `webbed`, `crawler`, `ggsql`, `httpfs_timeout_retry`, `http_stats`, `duckorch`, `quackscale`.

---

## Top 5 free wins

1. **`radio` + `CREATE STREAM` recipe** — document bus-in / SSE-out; zero C++; kills “we need WebSocket” for most push cases.  
2. **Dual rate-limit docs** — route `RATE LIMIT` (HTTP 429) **and** `rate_limit_fs` (GCRA on FS); one page, both catalog-backed.  
3. **`cronjob` + `CREATE QUEUE` recipe** — schedule `INSERT`/maintenance; workers already exist.  
4. **`jwt` claims in handlers** — `jwt_decode_payload` on `$headers` / token column; compose without waiting for full OIDC.  
5. **`FORMAT parquet` then `FORMAT arrow` (nanoarrow)** — only thin serdes C++; catalog already has IPC writers/`to_arrow_ipc`.

*(Close sixth: fix FEATURE_STATUS on gzip + document serve compression knobs — already implemented.)*

---

## Explicit non-substitutes (catalog does **not** replace)

| Want | Why catalog is not enough |
|------|---------------------------|
| Browser `ws://` on REST port | radio = client/bus; httplib has no Upgrade |
| Full GraphQL (N+1, subscriptions) | no GQL server extension |
| HTTP ETag / CDN edge | cache_httpfs is outbound FS, not response 304 |
| Arrow Flight **server** | airport/adbc are clients |
| uWebSockets/asio host | not in catalog |

---

## Re-query recipe

```sql
-- theme discovery
SELECT extension_name, count(*) AS blocks, count(query) AS with_query
FROM ext_catalog_clean
WHERE extension_name ILIKE '%radio%'
   OR content ILIKE '%WebSocket%'
GROUP BY 1 ORDER BY 2 DESC;

-- samples only
SELECT block_order, query
FROM ext_catalog_clean
WHERE extension_name = 'rate_limit_fs' AND query IS NOT NULL
ORDER BY block_order;
```

Via host:

```bash
duckdb -unsigned -init /dev/null -c "
LOAD quack;
FROM quack_query('quack://localhost:9494', \$\$ … \$\$,
  token=>'vori-quack-2026', disable_ssl=>true);
"
```
