# quackapi — STATUS (READ THIS FIRST)

> **LIVING DOC — single source of current truth.** Last verified against code + `git log`: **2026-07-09**.
> **Replaces `docs/FEATURE_GAP_MATRIX.md` (DELETED 2026-07-08).** That file was a *frozen 2026-07-02
> snapshot* that graded post-R2/B7 work as "HARD." A fresh agent read it as current state and
> hallucinated that shipped features (parallelism, concurrency, async access-log, keep-alive, DI,
> field-projection) were still open. This doc exists so that never happens again.
>
> **2026-07-09 audit note (learn from this):** the previous cut of THIS doc (2026-07-08) committed the
> SAME sin recursively — it listed **health #1907, DI #11143, and the Phase-0 auth gate as "building"
> / "open"** while `git log` showed all three already merged to `main` *before* the doc was written.
> Whoever wrote it transcribed the roadmap from the *specs*, not from the *code*. Corrected below.

## Rule for any agent reading this

1. **Do NOT infer "gaps" from any dated snapshot.** A doc with a date in its header is a photograph,
   not a live feed. If it grades a feature "HARD"/"open", assume it may be months of work stale.
2. **Before you call anything "building"/"open", run `git log --oneline -20` and grep the code.** The
   2026-07-08 staleness bug was exactly this: the doc claimed merged features were unbuilt. A
   `feat(#NNNN)` commit + a marker in `framework.sql`/`quackapi_extension.cpp` beats any prose here.
3. **Truth lives in the code, then the debt list — this file is only the index.** Verification order:
   1. `framework.sql` + `ext-cpp/src/quackapi_brain.cpp` + `ext-cpp/src/quackapi_extension.cpp` (the code).
   2. `git log --oneline` (what actually merged).
   3. `docs/BACKLOG.md` (the live debt/decision queue).
   4. `ext-cpp/B*_RESULT.md` / `*_RESULT.md` (dated proof-of-work with re-runnable evidence).
   5. This file (the map).
4. **A green claim without a re-runnable command is not evidence.** Every "shipped" row below carries
   one. Grok/subagent "all green" has been wrong twice — certify personally before believing.
5. **If this doc and the code disagree, the CODE wins — fix this doc.**

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
| Health / readiness / liveness + `/metrics` (**#1907**) | **Shipped (oracle + C)** — merged `e520a0c`/`e2520f0` | `CREATE HEALTH CHECK`, `/livez` `/readyz` in `framework.sql`; C `HEALTH` sugar in `quackapi_extension.cpp` |
| Request-scoped DI setup/teardown (**#11143 yield-DI**) | **Shipped (oracle + C)** — merged `962c1af`/`1435a6f` | `CREATE DEPENDENCY … SETUP … TEARDOWN …`; `dependencies`/`route_dependencies` + `run_dependency_phase`; C worker runs `setup; handler; teardown (always)` on `exec_con`; `test/di.test.sql` |
| Phase-0 auth gate — constant-time compare + oracle auth wiring | **Shipped** — merged `ec7ade6`/`602450d` | `_constant_time_str_equals` (XOR-fold, replaced `=`); authenticate→authorize wired into oracle `handle_request` |
| gzip response compression (= FastAPI `GZipMiddleware`) | **Shipped (main)** — source in `ext-cpp/src/quackapi_brain.cpp` (+`-lz`), built exit 0, live curl matrix certified | `Content-Encoding: gzip` + `Vary` on `/openapi.json`; <860B skipped; SSE untouched; regression 200s |
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

## Building — the FastAPI "most-wanted, closed unresolved" hit list (Wave A)

See `docs/FASTAPI_MOST_WANTED.md` (the pitch scoreboard) for the full upvote table. **Verified against
`git log` + code markers 2026-07-09** — 0 matches in `framework.sql`/`quackapi_extension.cpp` for the
rows below (except lifecycle, which is built-in-worktree-but-unmerged).

| 👍 | Issue | Feature | Mechanism | Status (code-verified) |
|---|---|---|---|---|
| 75 | #754 | First-class sessions | `CREATE SESSION` — server store IS a table; cookie issue/verify + CSRF | **not started in code** (0 markers); spec ready (`SESSION_CSRF_SPEC.md`), worktree `q-sessions` |
| 65 | #617 | Startup/shutdown lifecycle | `CREATE LIFECYCLE ON STARTUP\|SHUTDOWN AS <sql>` (drain already shipped) | **oracle built, UNMERGED** — on `fleet/lifecycle` (`5f250a1`), 0 markers in main; merge or fold in |
| 57 | #335 | OAuth2 Authorization-Code | `CREATE AUTH … AS OAUTH2` (redirect + token exchange + JWKS) | **WIP parked in stash** — `6088198` on `fleet/oauth-cpp` (+100 framework.sql, +45 tier-1); 0 markers in main; needs outbound-POST verdict + un-stash before continuing |
| 37 | #1428 | Keycloak/OIDC | same OAuth2 machinery + discovery URL | **not started** (rides #335) |

**Phase-0 auth gate: CLEARED** (was listed here as pending — it is not). `_constant_time_str_equals`
(XOR-fold) replaced DuckDB `=`, and authenticate→authorize is wired into the oracle `handle_request`
(merged `ec7ade6`/`602450d`). `BACKLOG.md` §3.3/§3.9 residuals are now polish, not a gate.

---

## Genuinely open / conceded — HONEST edges (named, not hidden; do NOT re-investigate as if new)

| Edge | Reality | Why it's not a "hole" |
|---|---|---|
| **Direct in-process TLS termination** | **Mechanism-limited.** Verified 2026-07-08: the vendored `duckdb_mbedtls` is a *crypto-only* static lib (HMAC/hash) — no `ssl.h`, no SSL/X.509/RNG; `mbedtls_ssl_init` is undefined against it. | **Proxy termination (Caddy / tailscale-serve) is v1 — that is uvicorn's own production answer.** Direct v2 needs a deliberate build decision (vendor full mbedTLS SSL, or vcpkg). `TLS_SPEC.md`. |
| **Async I/O fan-out inside one handler** | Event-loop model wins for many-slow-upstream-calls in a single request. GET fan-out is already parallel via `curl_httpfs`; POST fan-out mechanism is settling (`http_client.http_post` vs shellfs+curl `xargs -P`). | Conceded as *different, not equal* — documented, not contorted. Throughput concurrency (many clients) is separately **won** (B6/B7). |
| **Request-scoped DI — live-object lifetime** | **Setup/teardown IS shipped** (#11143 — `CREATE DEPENDENCY`, C worker runs `setup; handler; teardown (always)`). The *narrow* residual: a dependency that must hold a **live object across statements** (an open cursor, a streaming client) — the one-shot model guarantees teardown *runs*, not that the same object identity is threaded through, the way a Python generator frame holds it. | Setup/teardown-SQL sequencing: **done**. Object-identity-across-statements: honest residual, rarely needed, not overclaimed. `test/di.test.sql`. |
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
- `CREATE SUBSCRIPTION` — consume an upstream `ws://`/`wss://` feed or Redis channel (materialize
  `INTO` a table or run a per-message `HANDLER`) → **`radio` ext**. Verified round-trip 2026-07-06;
  spec complete, buildable now (`SUBSCRIPTION_SPEC.md`). **Nuance (verified): radio is a CLIENT only
  — it dials outbound; it does NOT accept inbound browser WebSocket connections.**
- WebSockets on the main port (browser → quackapi, `@app.websocket` equivalent) → **C-server work**
  (per-connection threads, `WS_SPEC.md`), NOT radio. Redis-style pub/sub (outbound) → `radio`.
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
| **Row-level security / multi-tenant** | hand-rolled `Depends` or Postgres RLS | `CREATE POLICY` (PERMISSIVE/RESTRICTIVE stacking, owner-from-token presets) | **PostgREST RLS, custom authz layers** | **shipped (oracle + C)** — Phase-0 wired oracle enforcement |
| **Realtime change feed / subscriptions** | Supabase Realtime, `broadcaster`, hand-rolled | `CREATE SUBSCRIPTION` (change-feed over shipped SSE) | **Supabase Realtime** | spec'd (`SUBSCRIPTION_SPEC.md`) |
| **Rate limiting** | `slowapi` (3rd-party) | `CREATE RATE LIMIT` (sliding window over the request-log table) | slowapi, nginx `limit_req` | named |
| **Background jobs / scheduled tasks** | Celery + Redis + beat | `CREATE JOB QUEUE` / `CREATE CRON` (cronjob ext) | **Celery + Redis, APScheduler** | named |
| **Full-text search endpoint** | Elasticsearch / Meilisearch + sync glue | `fts` ext, in-process `match_bm25` | **Meilisearch, Elastic-lite** | ext available |
| **Fuzzy / typo-tolerant search** | `rapidfuzz` glue | `rapidfuzz` ext, native | — | ext available |
| **Response caching + ETag/304** | `fastapi-cache` | `CACHE` clause + ETag over a result table | fastapi-cache | named |
| **Pagination** | `fastapi-pagination` | native `LIMIT/OFFSET` + `PAGINATE` clause | fastapi-pagination | near-trivial |
| **gzip compression** | `GZipMiddleware` (built-in) | C write-path gzip | — | **shipped (main, certified)** |
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

~~0. Certify + merge gzip~~ **DONE** — in main, built exit 0, curl matrix certified.
~~1. Phase-0 auth gate~~ **DONE** — constant-time compare + oracle auth wiring merged.

1. **`CREATE SUBSCRIPTION` (radio-backed)** — consume upstream `ws`/`redis` feeds; materialize
   `INTO` a table or handler-dispatch per message. Spec complete + radio round-trip verified
   (`SUBSCRIPTION_SPEC.md`). Ext-backed = cheap (registry + poller + DDL sugar). Open design calls
   to make first: handler-injection mechanism, admin-only security, restart durability. **Inbound
   browser WS is a SEPARATE C-server track (`WS_SPEC.md`) — radio is a client, it can't accept
   inbound connections.** Avoid `radio_subscriptions()` (UINT64 crash); use the per-URL helpers.
2. **Merge / fold `CREATE LIFECYCLE` #617** — oracle already built on `fleet/lifecycle` (`5f250a1`);
   certify + merge (or graft into the next feature branch) so it stops looking unbuilt.
3. **`CREATE API FOR TABLE`** — the flagship auto-CRUD (PostgREST / Supabase killer). Rides the
   already-shipped `CREATE POLICY` for row security so auto-exposing a table isn't a footgun.
4. **Remaining Wave-A most-wanted:** sessions #754 → OAuth2 #335 / OIDC #1428 (health #1907 + DI
   #11143 already shipped).

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
