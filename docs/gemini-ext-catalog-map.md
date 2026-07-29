# Gemini brainstorm × `ext_catalog` — what already exists

Live query on quack `:9494` `ext_catalog_clean` (2026-07-29).  
Thesis: **compose first**. New C++ only when catalog has no surface.

**Already in quackapi (PR #11 / gemini-surface):** RATE LIMIT route, FORMAT json|ndjson|csv, X-Request-ID, composition recipes doc.

---

## Theme map

| Gemini theme | Status | Extensions | Compose with quackapi | C++ in quackapi? |
|--------------|--------|------------|----------------------|------------------|
| **Outbound HTTP quality** | EXISTS_COMPOSE | **`curl_httpfs`** (58 blocks), `http_client`, `netquack` | Batteries already INSTALL/LOAD curl; force `http_client:='curl'` | Harden only: fail-loud when forced |
| **FS / API rate limits (I/O)** | EXISTS_COMPOSE | **`rate_limit_fs`** | Wrap httpfs/local FS: `rate_limit_fs_wrap` + quota — *orthogonal* to HTTP 429 on routes | No — document beside route RATE LIMIT |
| **Realtime / WebSocket *client* bus** | EXISTS_COMPOSE | **`radio`** | Handler SQL calls radio listen/publish; SSE stream can be fed from tables radio fills | No browser WS on httplib port; optional radio PRs if API gaps |
| **OAuth / OIDC** | EXISTS_PARTIAL | **`quack_oauth`** (153 blocks) | Compose in AUTH / token exchange handlers | Thin glue to wire CREATE AUTH → quack_oauth later |
| **Arrow / Flight / columnar IPC** | EXISTS_COMPOSE | **`nanoarrow`**, `arrow` (alias), **`adbc`**, `airport` | FORMAT arrow via nanoarrow; Flight clients via airport/adbc — not embed Flight server | FORMAT arrow next after parquet |
| **Parquet *read*** | ALREADY_CORE | core parquet | `read_parquet` in handlers | No |
| **Parquet *HTTP response body*** | NOT_IN_CATALOG as HTTP | core parquet write | `COPY … TO` / writer → response bytes | **Yes thin** — FORMAT parquet |
| **GraphQL engine** | NOT_IN_CATALOG | (sitting_duck has incidental “graphql” string; no GQL server) | Auto-schema from DuckDB catalog + single POST route | **Yes thin v0** — not months |
| **Code → routes** | EXISTS_PARTIAL | **`sitting_duck`**, **`parser_tools`**, **`sazgar`** | `quack_from_*` already uses sitting_duck; parser_tools for SQL shape; sazgar for external SQL dialects | Polish bridges, not rewrite |
| **AI / MCP gateway** | EXISTS_COMPOSE | **`ai`**, **`duckdb_mcp`** | Route calls `prompt()` / MCP server pragma | Recipe only |
| **Cron / schedules** | EXISTS_COMPOSE | **`cronjob`** | Outside request path; fill queue tables | Recipe with CREATE QUEUE |
| **Compression** | PARTIAL_CORE | miniz/zstd already in duckdb; batteries compression knobs | Honor Accept-Encoding (may already be partial) | Finish gzip wire if incomplete |
| **Distributed mesh** | EXISTS_COMPOSE | **`quack`**, **`airport`** | Handler `quack_query` / airport flights | Recipe |
| **ggsql charts** | EXISTS_COMPOSE | **`ggsql`** | Own mini HTTP for charts — parallel, not replace | Optional recipe |

---

## Free wins (compose / small C++)

1. **`curl_httpfs` force-fail** — product quality on linux/osx.  
2. **`FORMAT parquet`** — core writer → HTTP body (`PAR1`).  
3. **Document `rate_limit_fs` + route RATE LIMIT** — two layers (FS vs HTTP).  
4. **Document `radio` + CREATE STREAM** — bus in, SSE out (no fake browser WS).  
5. **Thin GraphQL v0** — catalog → types → `POST /graphql` → SQL (owner-driven).  
6. **`quack_oauth` recipe** — CREATE AUTH + token routes.  
7. **`nanoarrow` FORMAT arrow** after parquet.  
8. **`duckdb_mcp` / `ai` recipes** in extension-composition.md.  
9. **`cronjob` + QUEUE** recipe.  
10. **sitting_duck + parser_tools** → GraphQL/schema extract from existing FastAPI *or* from `quackapi_routes()` dump.

---

## Not “exists so skip building”

| Idea | Catalog does **not** replace |
|------|------------------------------|
| Browser `ws://api/…` on same port as REST | radio is client/bus; needs Upgrade host or sidecar |
| Full GraphQL mutations/subscriptions/N+1 | no GQL server extension |
| FORMAT parquet HTTP | no “HTTP parquet response” extension — only file IO |

---

## Top compose extensions (not all in composition doc yet)

`curl_httpfs`, `radio`, `quack_oauth`, `sitting_duck`, `parser_tools`, `sazgar`, `nanoarrow`, `adbc`, `airport`, `quack`, `cronjob`, `rate_limit_fs`, `http_client`, `netquack`, `duckdb_mcp`, `ai`, `pdf`, `tera`, `ggsql`

---

## GraphQL v0 (doable with catalog + routes)

No GraphQL *server* in catalog → implement thin in quackapi:

```text
DuckDB information_schema / quackapi_routes()
  → GraphQL schema (types = tables or route result shapes)
POST /graphql { query }
  → parse query (hand or sitting_duck/parser_tools later)
  → SELECT … FROM table / invoke route SQL
  → JSON { data }
```

`sitting_duck` / `parser_tools` help **ingest** existing GQL/TS clients or validate SQL; they do not remove the need for a request entrypoint.
