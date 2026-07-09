# quackapi — STATUS (READ THIS FIRST)

> **LIVING DOC — single source of current truth.** Last verified against code: **2026-07-08**.
> **Replaces `docs/FEATURE_GAP_MATRIX.md` (DELETED 2026-07-08).** That file was a *frozen 2026-07-02
> snapshot* that graded post-R2/B7 work as "HARD." A fresh agent read it as current state and
> hallucinated that shipped features (parallelism, concurrency, async access-log, keep-alive, DI,
> field-projection) were still open. This doc exists so that never happens again.

## Rule for any agent reading this

1. **Do NOT infer "gaps" from any dated snapshot.** A doc with a date in its header is a photograph,
   not a live feed. If it grades a feature "HARD"/"open", assume it may be months of work stale.
2. **Truth lives in the code, then the debt list — this file is only the index.** Verification order:
   1. `framework.sql` + `ext-cpp/src/quackapi_brain.cpp` + `ext-cpp/src/quackapi_extension.cpp` (the code).
   2. `docs/BACKLOG.md` (the live debt/decision queue — kept current).
   3. `ext-cpp/B*_RESULT.md` / `*_RESULT.md` (dated proof-of-work with re-runnable evidence).
   4. This file (the map).
3. **A green claim without a re-runnable command is not evidence.** Every "shipped" row below carries
   one. Grok/subagent "all green" has been wrong twice — certify personally before believing.
4. **If this doc and the code disagree, the CODE wins — fix this doc.**

---

## Perf — the current crown (B7, 2026-07-05; proof: `ext-cpp/B7_RESULT.md`)

Instance pool (1 writer + per-worker `:memory:` read replicas) + mimalloc interposition beat the
`/search` scaling wall (which was the macOS system allocator + 16-conns-to-one-instance topology,
**not** tunable knobs — B4/B5 killed the knob theory at ≤3%).

| endpoint | quackapi req/s | FastAPI best-ever cell | ratio |
|---|---|---|---|
| `/search` (real BM25-ish work, c64) | **49,091** | 21,824 | **2.2×** |
| `/users/1` (c64 keep-alive) | **136,438** | — | — |
| `/health` (c64 keep-alive) | **134,340** | — | — |

Re-run: `ext-cpp/scripts/serve.sh` (boots with mimalloc + pool) then the bench in `bench/`.
Known honest ceiling: DuckDB per-query task-setup tax at sub-ms scale (~4.8–6.1× of a ~13× ceiling on
12P+4E) — documented, not config-fixable. Next authorized ceiling-breaker: set-based request batching,
gated at **≥59k /search** (`BACKLOG.md` §4).

---

## Shipped & self-verified (do NOT reopen as if unbuilt)

| Axis | Status | Proof / re-run |
|---|---|---|
| Routing + path/query/header/cookie/form params | Parity, byte-tested | `test/` R1 suite; `parity_b2.sh` |
| Validation + FastAPI-exact 422 (incl. `input`) | Parity (oracle + C, 33-case byte parity) | `test/tier1_handle_request.test.sql` |
| OpenAPI + Swagger UI | Parity, arguably better (generation is a `SELECT`) | boot `serve_brain`, curl `/openapi.json` `/docs` |
| `CREATE ROUTE` live DDL registration | Strictly better (no restart; FastAPI needs one) | `quackapi_extension.cpp` ParserExtension |
| Response field include/exclude (**FastAPI #1357**) | **Shipped** — `CREATE ROUTE … FIELDS(INCLUDE… \| EXCLUDE…)` | oracle 156/156; `_apply_field_projection` |
| Auto-HEAD for every GET (**#1773**) | **Shipped** (R4) | parity 40/40, tier-1 112/112, live curl |
| SSE / chunked streaming | Have, verified | curl `-N` on the stream route |
| Multipart uploads (text-safe v1) | Have (R3); binary/base64 = v2 planned | `MULTIPART_SPEC.md` |
| HEAD/OPTIONS/405 + `Allow`, graceful SIGTERM drain, access logs | Have (R4, live-verified) | `CHRONICLE.md` R4 |
| Keep-alive persistent connections | Shipped + self-verified | B7 numbers above |
| Parallelism + concurrency scaling | **Shipped** (B6/B7 — allocator + instance pool) | `ext-cpp/B6_RESULT.md`, `B7_RESULT.md` |
| Async access-log ring buffer | Built (in `quackapi_brain.cpp`: `logger_thread_main`, 65536-slot ring) | grep `g_log_ring` in brain.cpp |
| JWT HS256 / API-key / policy auth (C enforcement) | Shipped + live-verified 2026-07-06 | `ext-cpp/A1_AUTH_RESULT.md` (11/11 curl) |
| Perf "poor performance" rebuttal (**#1664**) | Answered | B7 + `bench/BENCH_HEADTOHEAD.md` |
| HTTPBearer 401-not-403 (**#10177**) | Correct | `A1_AUTH_RESULT.md` matrix |
| Pydantic↔SQLAlchemy bridge (**#214**) | Dissolved (handlers are SQL; no ORM to bridge) | — |

---

## Built, pending certification (NOT merged to main)

| Feature | Where | Evidence | Gate to merge |
|---|---|---|---|
| **gzip response compression** (= FastAPI `GZipMiddleware`) | worktree `q-gzip` (`ext-cpp/src/quackapi_brain.cpp` +124, `CMakeLists.txt` `-lz`) | grok curl matrix in `/tmp/grok_gzip.log`: `Content-Encoding: gzip` on `/openapi.json`, decode-to-valid-JSON, no-header→plaintext, tiny-body skipped (<860B), SSE untouched, `/health`/`/users`/`/search` regression 200 | **My** cert: oracle tier-1 + fresh-port boot + curl matrix, then merge serially |

---

## Building — the FastAPI "most-wanted, closed unresolved" hit list (Wave A)

See `docs/FASTAPI_MOST_WANTED.md` (the pitch scoreboard) for the full upvote table.

| 👍 | Issue | Feature | Mechanism | Status |
|---|---|---|---|---|
| 75 | #754 | First-class sessions | `CREATE SESSION` — server store IS a table; cookie issue/verify + CSRF | building (`SESSION_CSRF_SPEC.md`) |
| 65 | #617 | Startup/shutdown lifecycle | `CREATE LIFECYCLE ON STARTUP\|SHUTDOWN AS <sql>` (drain already shipped) | partial→building |
| 62 | #1907 | readiness/liveness/health | `CREATE HEALTH CHECK` + `/livez` `/readyz` + `/metrics` as a `SELECT` | building |
| 57 | #335 | OAuth2 Authorization-Code | `CREATE AUTH … AS OAUTH2` (redirect + token exchange + JWKS) | building |
| 37 | #1428 | Keycloak/OIDC | same OAuth2 machinery + discovery URL | building |

**Phase-0 gate before any "secure" auth claim** (`BACKLOG.md` §3.3/§3.9): (1) real XOR-fold
constant-time compare replacing DuckDB `=`; (2) wire the authenticate→authorize stage into the
oracle `handle_request` (currently only the C server enforces).

---

## Genuinely open / conceded — HONEST edges (named, not hidden; do NOT re-investigate as if new)

| Edge | Reality | Why it's not a "hole" |
|---|---|---|
| **Direct in-process TLS termination** | **Mechanism-limited.** Verified 2026-07-08: the vendored `duckdb_mbedtls` is a *crypto-only* static lib (HMAC/hash) — no `ssl.h`, no SSL/X.509/RNG; `mbedtls_ssl_init` is undefined against it. | **Proxy termination (Caddy / tailscale-serve) is v1 — that is uvicorn's own production answer.** Direct v2 needs a deliberate build decision (vendor full mbedTLS SSL, or vcpkg). `TLS_SPEC.md`. |
| **Async I/O fan-out inside one handler** | Event-loop model wins for many-slow-upstream-calls in a single request. GET fan-out is already parallel via `curl_httpfs`; POST fan-out mechanism is settling (`http_client.http_post` vs shellfs+curl `xargs -P`). | Conceded as *different, not equal* — documented, not contorted. Throughput concurrency (many clients) is separately **won** (B6/B7). |
| **Request-scoped DI with setup/teardown** | The real architectural tear of a stateless one-shot SQL dispatch model (FastAPI #11143/#10719/#1474). | Named honest edge; `probes/6_di_setup_teardown.sql`. Not overclaimed. |
| **WebSockets on the main HTTP port** | Transport prototyped; app-layer wiring pending. Design decision is **radio** (the DuckDB realtime ext) + `CREATE ROUTE … WS`. | Spec'd (`WS_SPEC.md`); a decided design, not a missing capability. |
| **Multipart binary** | v1 text-safe shipped; binary via base64 vs streaming parser is the open call. | `MULTIPART_SPEC.md` §6/§7. |
| **Horizontal scale + ecosystem/Stack-Overflow familiarity** | Single-process blast radius until Quack multi-writer; smaller SO corpus than FastAPI. | Conceded, countered by the whole DuckDB extension ecosystem being the "standard library" + a cleaner `INSTALL quackapi FROM community` story. |

---

## What people actually want next — the roadmap that IS the pitch

The through-line: **almost every one of these is a separate product, a paid SaaS, or a
pip-install-plus-glue in FastAPI-land — and it collapses to a single `CREATE …` statement here,
because the framework IS the database.** This is the "categorically superior in the db ways" claim,
made concrete. (Wishlist, not a grade — safe from staleness. Source: `BACKLOG.md` §2, idea-ledger.)

**The composability point — this IS the moat, and it means most of these are NOT builds.** An
existing DuckDB extension already does the hard part; the "work" is `LOAD <ext>` + a thin `CREATE …`
sugar layer wired into `handle_request`. What FastAPI needs a pip **plus a running service** for
(Redis, a broker, a search cluster, an S3 SDK), quackapi gets from `LOAD`.

**Tier 1 — undercut whole products (mostly ext-backed):**
- `CREATE SUBSCRIPTION` (Supabase Realtime) **+** WebSockets on the main port **+** Redis-style
  pub/sub → **`radio` ext** (message bus + WS). One extension collapses all three.
- `CREATE API FOR TABLE` (PostgREST / Supabase auto-API) → SQL over the catalog (a real build);
  for a Postgres backend the **`postgres` ext** passthrough undercuts PostgREST directly.
- `CREATE POLICY` (PostgREST RLS) → SQL predicate + **`crypto` ext** (`crypto_hmac`) for token verify.
- `CREATE JOB QUEUE` / `CREATE CRON` (Celery + Redis) → **`cronjob` ext** (scheduling) + `radio` (queue).
- Full-text / fuzzy search endpoints (Meilisearch / Elastic-lite) → **`fts` + `rapidfuzz` exts**.
- `CREATE STORAGE` S3 (boto3) → **`httpfs` ext** (native S3). · `CREATE WEBHOOK` / outbound → **`http_client`** / httpfs / curl.

**Tier 2 — thin native SQL sugar (no ext, no service):** `CREATE RATE LIMIT` (slowapi) · `CACHE`+ETag
(fastapi-cache) · `PAGINATE` (fastapi-pagination) · `/metrics` as a `SELECT` over log tables
(prometheus-instrumentator) · `CREATE CORS` · versioning · migrations auto-diff.

**Still a genuine build (no ext covers it):** Admin UI (embedded HTML) · SMTP/email (needs an ext or
a CNI) · `IDEMPOTENCY KEY` / `CREATE TOKEN BLACKLIST` (dedup table, maybe **`bitfilters`**).

**Wave A — FastAPI's most-wanted-closed:** sessions #754 · lifecycle #617 · health #1907 ·
OAuth2 #335 · OIDC/Keycloak #1428.
**Wildcard nobody else has:** the FastAPI **offboarder** — `read_ast` a FastAPI repo → generate
`CREATE ROUTE` + `param_schema`, NEEDS_REVIEW markers for the un-migratable. Attacks switching cost.

| What people want | In FastAPI-land you reach for | quackapi mechanism | What it undercuts | Status |
|---|---|---|---|---|
| **Auto REST/CRUD from a table** | `fastapi-crudrouter` (unmaintained) → drop to PostgREST/Supabase | `CREATE API FOR TABLE` | **PostgREST, Supabase auto-API** | spec'd (`TABLE_API_SPEC.md`) |
| **Row-level security / multi-tenant** | hand-rolled `Depends` or Postgres RLS | `CREATE POLICY` (PERMISSIVE/RESTRICTIVE stacking, owner-from-token presets) | **PostgREST RLS, custom authz layers** | C-enforce shipped; oracle wiring pending |
| **Realtime change feed / subscriptions** | Supabase Realtime, `broadcaster`, hand-rolled | `CREATE SUBSCRIPTION` (change-feed over shipped SSE) | **Supabase Realtime** | spec'd (`SUBSCRIPTION_SPEC.md`) |
| **Rate limiting** | `slowapi` (3rd-party) | `CREATE RATE LIMIT` (sliding window over the request-log table) | slowapi, nginx `limit_req` | named |
| **Background jobs / scheduled tasks** | Celery + Redis + beat | `CREATE JOB QUEUE` / `CREATE CRON` (cronjob ext) | **Celery + Redis, APScheduler** | named |
| **Full-text search endpoint** | Elasticsearch / Meilisearch + sync glue | `fts` ext, in-process `match_bm25` | **Meilisearch, Elastic-lite** | ext available |
| **Fuzzy / typo-tolerant search** | `rapidfuzz` glue | `rapidfuzz` ext, native | — | ext available |
| **Response caching + ETag/304** | `fastapi-cache` | `CACHE` clause + ETag over a result table | fastapi-cache | named |
| **Pagination** | `fastapi-pagination` | native `LIMIT/OFFSET` + `PAGINATE` clause | fastapi-pagination | near-trivial |
| **gzip compression** | `GZipMiddleware` (built-in) | C write-path gzip | — | **built, pending cert** |
| **Metrics / Prometheus** | `prometheus-fastapi-instrumentator` | `/metrics` as a `SELECT` over log tables | instrumentator | named |
| **Admin panel** | `sqladmin`, `fastapi-admin` | embedded single-file Admin UI | sqladmin | spec'd (`ADMIN_UI_SPEC.md`) |
| **File storage (S3/local)** | `boto3` glue | `CREATE STORAGE` (httpfs native) | boto3 glue | named |
| **Outbound webhooks** | hand-rolled `httpx` | `CREATE WEBHOOK` (http_client / curl fan-out) | — | named |
| **Idempotency keys** | no good FastAPI story | `IDEMPOTENCY KEY` clause (dedup table) | — | named |
| **Email / SMTP + templates** | `fastapi-mail` | SMTP + templated emails | fastapi-mail | named |
| **Migrations** | `alembic` | schema-is-the-DB, auto-diff | alembic | named |
| **CORS** | `CORSMiddleware` (limited) | `CREATE CORS` | — | named |
| **API versioning** | `fastapi-versioning` | route version metadata | — | named |
| **Token blacklist / revocation** | hand-rolled | `CREATE TOKEN BLACKLIST` | — | named |
| **Typed JS/TS client SDK** | `openapi-generator` glue | SDK from the same OpenAPI `SELECT` + CI validation | — | named |
| **The FastAPI "offboarder"** | (no equivalent) | `read_ast` a FastAPI repo's `.py` → generate `CREATE ROUTE` + `param_schema`; NEEDS_REVIEW markers for imported models / class-based views | migration-cost-to-switch | prototype proven |

---

## Build order — what we make next

Weighted by leverage × cost, where **ext-backed = cheap** (`LOAD` + sugar, not reimplementation).

0. **Certify + merge gzip** — built in worktree `q-gzip`, pending my cert. Boot + run the curl matrix
   myself before merge; grok "all green" is not evidence. Fast, closes in-flight work.
1. **Phase-0 auth gate** (`BACKLOG.md` §3.3/§3.9) — real XOR-fold constant-time compare + wire
   authenticate→authorize into the oracle `handle_request` (only C enforces today). Small; unblocks
   everything auth/session/policy. Uses the **`crypto` ext** already in play.
2. **radio cluster** — `CREATE SUBSCRIPTION` (Supabase Realtime killer) + WebSockets on the main
   port, both riding the **`radio` ext** (bus + WS). High leverage, low cost — mostly DDL sugar +
   wiring since radio owns the transport. Verify radio's server-side WS surface before building.
3. **`CREATE API FOR TABLE`** — the flagship auto-CRUD (PostgREST / Supabase killer). Rides #1/#2
   policy for row security so auto-exposing a table isn't a footgun.
4. **Wave-A most-wanted:** sessions #754 → health/readiness #1907 → lifecycle #617 → OAuth2 #335 / OIDC #1428.

Each feature: SUGAR-FIRST DDL, oracle tier-1 + `parity_b2.sh` (oracle == C) + a live curl matrix on a
fresh 1845x port, **certified by me before merge**.

## Doc map (so you never read a frozen doc as live again)

- **`docs/STATUS.md`** — this file. Current state + roadmap. Read first.
- **`docs/BACKLOG.md`** — live debt, deferred gates, Alok decision queue. Second.
- **`docs/FASTAPI_MOST_WANTED.md`** — the upvote scoreboard (the pitch artifact).
- **`docs/CHRONICLE.md`** — the narrative/history + learning apparatus (regen: `docs/build_pdf.sh`).
- **`ext-cpp/B*_RESULT.md`, `ext-cpp/A1_AUTH_RESULT.md`** — dated proof-of-work with evidence.
- **`docs/specs/*`** — per-feature specs (SESSION_CSRF, TABLE_API, ADMIN_UI, TLS, WS, MULTIPART, …).

Mirror this doc to Notion (personal-projects DB) per the docs rule.
