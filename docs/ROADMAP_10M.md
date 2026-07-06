# ROADMAP_10M — the swing-for-the-fences plan

**Directive (Alok, 2026-07-05):** "i want something super powerful… worthy of a c++ community
extension and fills a need… all the shit fastapi ppl complain about it doesn't do but could/should.
I want all that shit. Not joking about swinging for the fences — 'why can't 10 million ppl use
this' is the ceiling."

**This doc is the synthesis of the full research corpus:** the six FastAPI-pain reports
(`research/fastapi-pain-2026-07-02/01–06`), `FEATURE_GAP_MATRIX.md`, the DDL-fleet reports, the
existing specs (`specs/`), plus two new research sweeps
(`research/instant-backend-rivals-2026-07-05.md`;
`research/fastapi-pain-2026-07-02/07-bolton-ecosystem.md`). Every complaint or capability maps to
a quackapi answer with a status tag:

- **SHIPPED** — built, measured, in the tree today
- **SPECCED** — full spec exists in `docs/specs/`, not yet built
- **NOT-SPECCED** — named primitive, needs a spec
- **HONEST-EDGE** — we don't do it; we say so loudly (edges.md / "When NOT to use quackapi")

---

## 1. The category verdict: what "10 million people" actually means

The 10M-ceiling category is **instant backend / backend-in-a-box**, not "web framework."
Frameworks are chosen by language communities; instant backends are chosen by anyone with an app
idea. The kings:

| product | proof | what actually drove adoption |
|---|---|---|
| PocketBase | 59.4k stars, still pre-1.0, bus-factor 1 | ONE file, zero deps, admin UI = 60-second demo, Firebase-refugee framing, SQLite-as-feature |
| Supabase | the hosted king | Postgres-is-the-product, PostgREST instant REST, RLS, GoTrue auth, realtime CDC. **Self-host = the category's biggest documented pain** (10+ containers, nerve-racking upgrades) |
| PostgREST / Hasura | beloved but capped | schema reflection + DB-native RLS — but they're *components*, not products, and business logic always escapes to a second service |
| Datasette | the cautionary tale | genius engine, read-only core + no auth + "data publishing" framing = niche forever. Writes + auth + app-backend framing turns the same idea into PocketBase-scale adoption |

**The DuckDB lane is verified EMPTY.** The community `httpserver` ext is an experimental
query-over-HTTP endpoint (no routing, no framework). The official Quack protocol (May 2026) is
DuckDB↔DuckDB infrastructure — something we *ride* (multi-writer story), not a competitor.
`duckdb_featureserv` is geo-only. Nobody has built the instant backend on the fastest-growing
database of the decade.

**Positioning line:** ***"PocketBase, but your database is a warehouse."***
One extension. `LOAD quackapi; SELECT quackapi_serve('app.db', 8000);` and you have a REST API,
auth, admin UI — and unlike every SQLite/Postgres rival, your backend natively reads Parquet,
Iceberg, S3, and runs OLAP at columnar speed. Analytics isn't a bolt-on ETL pipeline; it's a
route.

Exploitable openings, ranked: PocketBase bus-factor/pre-1.0 fatigue · zero analytics story in the
entire category · SQLite write ceiling · Supabase self-host pain.

---

## 2. Where we stand today (honest inventory, 2026-07-05)

**Shipped and measured** (B1–B7, R1–R3 waves; every number run by us on the live server):

- C++ community extension, single artifact, cold start <100 ms
- CREATE ROUTE / DROP ROUTE DDL — routes are rows, app state is queryable (`SELECT * FROM routes`)
- Instance pool (1 writer + N `:memory:` replicas) + mimalloc: **49,091 req/s** on /search c64
  (2.2× the best FastAPI cell *with no database at all*), /users/1 136k, /health 134k (ab-limited)
- Read-your-writes guarantee across the pool; 50 concurrent writes under 47.6k req/s read load,
  zero lost
- Validation: TRY_CAST + constraints, 422 byte-compatible with FastAPI (`input` + `ctx` exact)
- OpenAPI-as-a-SELECT + Swagger UI; operationId = route names (kills FastAPI's ugliest wart)
- SSE/streaming (kind=stream, chunked), background dispatch, middleware chain, header/cookie/form
  params, multipart v1 (text-safe), access logging at full throughput, graceful SIGTERM,
  127.0.0.1 default bind
- Conformance: 87-case differential suite vs live FastAPI, 54/54 MATCH on implemented surface
- `make test` 67/67; zero-warning build

**Specced, not built:** TLS (`TLS_SPEC`), WebSockets on the main port (`WS_SPEC`), auth/policy
(`CREATE_POLICY_AUTH_SPEC`), multipart binary v2 (`MULTIPART_SPEC`), ops polish
(`POLISH_OPS_SPEC`), request-surface leftovers (`R1_REQUEST_SURFACE_SPEC`).

**Honest edges that stand:** async upstream I/O fan-out, HTTP/2, polymorphic
discriminated-union validation, open-transaction-per-request DI. Documented, not hidden.

---

## 3. The ceiling checklist — category MUSTs mapped to quackapi

From the rivals research: what EVERY member of the category ships. Never compromise on MUST.

| # | MUST (category table stakes) | quackapi answer | status |
|---|---|---|---|
| 1 | Instant CRUD for any table — filter/sort/paginate/expand | `CREATE API FOR TABLE users` (auto-routes; expand = follow FKs) | **NOT-SPECCED** — the single highest-leverage unbuilt feature |
| 2 | Auth: email/password + ≥5 OAuth providers + reset/verify + JWT issuance | CREATE_POLICY_AUTH_SPEC covers JWT verify/claims; issuance + OAuth flows + email flows are new | **SPECCED (partial)** |
| 3 | Declarative row-level policies (RLS analog) | `CREATE POLICY` DDL — claims flow as bound MAP, policy is a SQL predicate | **SPECCED** |
| 4 | Admin dashboard served from the artifact | `/­_admin` routes served by the extension itself (schema browser, data grid, route/policy editor — it's all just tables) | **NOT-SPECCED** — the 60-second-demo adoption lever |
| 5 | Realtime subscriptions (SSE) | SSE primitive SHIPPED; subscriptions = change-feed on tables (`CREATE SUBSCRIPTION`) | **NOT-SPECCED** (primitive shipped) |
| 6 | File storage (local + S3) + serving | httpfs already in DuckDB; `CREATE STORAGE` DDL + multipart v2 | **NOT-SPECCED** (multipart v2 SPECCED) |
| 7 | Versioned migrations, auto-diffed | schema IS tables — diff `duckdb_tables()` snapshots; `quackapi_migrate()` | **NOT-SPECCED** |
| 8 | Backup one-liner + durability story | `EXPORT DATABASE` exists; document + `quackapi_backup()` wrapper; Litestream-analog doc | **NOT-SPECCED** (small) |
| 9 | Typed JS/TS SDK generated from OpenAPI | our OpenAPI is already a SELECT; ship `npx quackapi-client` off the standard generators | **NOT-SPECCED** (mostly glue + CI validation) |
| 10 | TLS, logs, health out of the box | access logs SHIPPED; TLS SPECCED; `CREATE HEALTH CHECK` NOT-SPECCED (FastAPI #7242, 200+ upvotes) | **mixed** |
| 11 | Single-artifact zero-config boot | SHIPPED — this is the thing we already are | **SHIPPED** |

**SHOULD (most of the category has it):** JS-VM-or-webhook escape hatch (ours is better: the
escape hatch is *SQL macros + HTTP client from SQL*) · `CREATE CRON` scheduling · SMTP + templated
auth emails · bulk CSV/Parquet import-export (native DuckDB — trivially ours) · rate-limit DDL ·
multi-writer via Quack protocol · Dart SDK later.

**DIFFERENTIATOR (nobody in the category has ANY of these — this is the fences part):**

- **Analytics routes** — dashboards/cohorts/funnels as routes, zero ETL: "your admin UI includes a warehouse"
- **Lakehouse routes** — a route over Parquet/Iceberg/S3 via httpfs; your API serves data that isn't even *in* the database
- **Everything-is-SQL introspection** — routes, policies, logs, metrics all queryable tables
- **In-DB FTS + vector search** — "Algolia + pgvector included"
- **Zero-copy pandas/polars access** to the same file the API serves
- **Columnar performance** — 49k req/s from one process, receipts in BENCH_HEADTOHEAD
- **Time-travel/audit reads** where snapshots allow

---

## 4. FastAPI complaint → quackapi answer (master table)

Condensed from reports 01–06. **(A)** = architectural crack we beat by construction,
**(B)** = execution gap we must build, **(C)** = deliberate tradeoff we document.

### Structural wins — already true by construction (A, SHIPPED)

| FastAPI pain | quackapi answer |
|---|---|
| sync-in-async starvation, 40-thread AnyIO cliff, GIL | no event loop, no GIL — thread-per-conn + instance pool, measured monotonic scaling |
| Pydantic hot path = 60% of request time | TRY_CAST validation inside the engine |
| BaseHTTPMiddleware 3.7× cliff | middleware chain is SQL, measured flat |
| three-model DTO problem (Create/Read/Update classes) | table = schema = response; biggest, clearest win (report 04) |
| v1→v2 churn immunity | validation semantics are DuckDB's, versioned with the engine |
| response_model double-serialization | one-pass serialization |
| operationId ugliness | route names ARE operationIds (DDL) |
| worker memory × N, 800 ms–2.5 s cold starts | one process, <100 ms |
| app structure chaos / "magic" import-time errors | app state is a queryable catalog; errors are rows |
| dependency_overrides global-dict test flakiness (#1 flaky-suite source) | `:memory:`-per-test; SQL-oracle testing — headline pitch (report 03) |
| lifespan silent failures | boot is a SQL script; failures are errors at DDL time |
| CORS `'*'`+credentials footgun | rejected at DDL parse time |

### Execution gaps — must build (B)

| complaint (evidence) | primitive | status |
|---|---|---|
| auth is DIY + CVE graveyard (python-jose CVE-2024-33663; 3-year bad-doc gap) | `CREATE POLICY` + JWT verify + **issuance/OAuth/email flows** | SPECCED (partial) |
| JWT revocation impossible without Redis | `CREATE TOKEN BLACKLIST` | NOT-SPECCED |
| rate limiting = slowapi + Redis + footguns | `CREATE RATE LIMIT` (+ BACKEND clause) | NOT-SPECCED |
| health checks (discussion #7242, 200+ 👍) | `CREATE HEALTH CHECK` | NOT-SPECCED |
| observability DIY | `SET http_trace_propagation` → X-Request-ID into query_log; metrics endpoint | NOT-SPECCED |
| Celery/APScheduler/ARQ pain (report 05 §8 — the single most-installed pile) | `CREATE CRON` + `CREATE JOB QUEUE` (MAX_RETRIES/BACKOFF/DEAD_LETTER/VISIBILITY_TIMEOUT) | NOT-SPECCED — "the Celery killer" |
| multi-tenancy DIY | `CREATE TENANT` | NOT-SPECCED |
| external stateful resources — "the DI gap that matters most" (report 03) | `CREATE CONNECTION` | NOT-SPECCED |
| WebSockets | main-port mount | SPECCED |
| TLS | native | SPECCED |
| binary multipart | v2 | SPECCED |

### Deliberate tradeoffs — document, don't fake (C, HONEST-EDGE)

Polymorphic/discriminated-union validation at depth · recursive schemas · context-aware
transform-on-validate · open transaction across a request · async upstream fan-out · HTTP/2
(until built) · Python-ecosystem interop. Each stays in edges.md with a probe.
**Plus report 05 §10's own-medicine list for the README's "When NOT to use quackapi" section:**
single-process blast radius, horizontal scaling story (until Quack multi-writer), OLAP-OOM risk
from hostile queries, extension trust boundary.

### Danger rails (report 06 — never break these while swinging)

1. **OpenAPI 3.1 compliance validated in CI** (openapi-validator + generator-cli as gates, golden
   spec diff) — silently breaking it kills 50+ downstream tools including our own SDK story (MUST #9).
2. Conformance suite stays green on every wave — the differential harness IS the regression net.

---

## 5. Bolt-on ecosystem sweep — what production FastAPI apps actually pip-install on top

Full sourced report: `research/fastapi-pain-2026-07-02/07-bolton-ecosystem.md`. The meta-finding
first, because it IS our pitch: **the top-recurring complaint isn't any one feature — it's that
the vacuum gets filled by 25–150★ micro-packages carrying 25K–1M downloads/month that then get
abandoned** (fastapi-versioning, fastapi-cache2, asgi-idempotency-header,
content-size-limit-asgi). quackapi's answer is structural: every battery lives in ONE artifact
versioned with the engine. (Honesty note: the same governance/bus-factor complaint — FastAPI
#4263, 87👍 — applies to us at bus-factor 1. So does PocketBase's. Ship anyway; document it.)

The agent's top-10 ranked gaps (complaint frequency × breadth) mapped to quackapi:

| rank | FastAPI gap (evidence) | quackapi answer | status |
|---|---|---|---|
| 1 | testing harness — rollback-per-test folklore, pytest-asyncio 0.23 breakage plague, pytest-django 21M dl/mo proves demand | `:memory:`-per-test + SQL-oracle testing already structural; ship a first-party test harness doc + fixtures | **SHIPPED (core) / NOT-SPECCED (harness packaging)** |
| 2 | /metrics — prometheus-instrumentator 14M dl/mo ≈ every prod deploy; K8s probes 62👍 unshipped | `/metrics` route over query_log/access_log tables (metrics ARE tables); pairs with `CREATE HEALTH CHECK` | NOT-SPECCED (cheap — data already exists) |
| 3 | body limits / timeouts / slowloris — wontfix across uvicorn/starlette, CVE-2026-54283 in the partial fix, official answer "use nginx" | limits/deadlines in the C++ server core: Content-Length + streamed-byte 413, header-read deadline, per-request wall-clock timeout | NOT-SPECCED — **we own the server, they don't** |
| 4 | migrations — Alembic 188M dl/mo monopoly, rename data loss, multiple-heads; tiangolo's roadmap admits the gap | auto-diff of catalog snapshots + versioned migration files (MUST #7) | NOT-SPECCED |
| 5 | pagination — fastapi-pagination 3.39M dl/mo; in-core PR REJECTED; no standard envelope | standard envelope + cursor/offset in `CREATE API FOR TABLE` (MUST #1) | NOT-SPECCED (folds into Wave A) |
| 6 | sessions/CSRF — **highest-👍 feature issue in FastAPI history (#754, 75👍)**, users defecting to Django-Ninja over it | `CREATE SESSION` (server-side store is… a table) + CSRF middleware in the auth wave | NOT-SPECCED — added to Wave A/B scope |
| 7 | admin panel — loudest "what I miss from Django"; 30 min (Django) vs 2–3 days (FastAPI) quantified; 4-way fragmented ecosystem, leader stale | `/_admin` served from the extension (MUST #4) | NOT-SPECCED — Wave A centerpiece |
| 8 | scaffolding/structure — conventions README (17.6k★) out-stars every generator; answer is cloning a 44k★ template | nothing to scaffold: an app is one DDL file; `quackapi init` emits it | SHIPPED (structural) / trivial CLI sugar |
| 9 | SDK generation — FastAPI docs ship workarounds for their own operationId/`Body_*` naming | clean operationIds SHIPPED (route names); ship generator glue + CI validation (MUST #9) | partial |
| 10 | response caching/ETag — 1M dl/mo on a stale package w/ 108 open issues; zero conditional-request support in core | `CACHE` clause on routes + ETag/If-None-Match→304 in core | NOT-SPECCED |

Below the top 10 but high-leverage for us:

- **Outbound webhooks** — gap big enough that a VC-backed company (svix) exists; FastAPI's
  `app.webhooks` is documentation-only. `CREATE WEBHOOK` (durable queue + backoff + HMAC signing,
  standard-webhooks spec) rides the same machinery as `CREATE JOB QUEUE`. NOT-SPECCED, Wave C.
- **Idempotency keys** — no package ever won (nothing >30★ in 5 years; everyone hand-rolls Redis
  `SET NX EX`). An `IDEMPOTENCY KEY` route clause is a table + a unique constraint for us.
  NOT-SPECCED, Wave C.
- **Audit history / soft deletes** — Django-analog demand ratios 7×/12× vs the SQLAlchemy
  ecosystem's half-maintained options; SQLAlchemy formally refused in-core. `CREATE AUDIT` shadow
  tables + soft-delete route semantics; feeds DIFFERENTIATOR #26 (time-travel reads). Wave D.
- **API versioning** — most-starred package abandoned since 2021; discussion open since 2019.
  Route metadata filter (report 04 already sketched it). Small, Wave C.
- **HTMX/full-stack** — big enough to spawn FastHTML (7k★). We already serve HTML routes +
  templates; add flash messages, static files with Cache-Control, HTML-aware 422 rendering. Wave D.
- **File storage** — UploadFile spools to RAM (13s vs 0.6s aiohttp measured); canonical fix is a
  gist. Streaming multipart v2 + `CREATE STORAGE` (MUST #6), Wave B.
- **i18n of 422 messages** — pydantic #322 open since 2018 (English hardcoded in Rust core). Our
  messages come from a table — translatable by INSERT. Cheap novelty, Wave E.
- **GraphQL** — Starlette removed it from core; strawberry integration has open DI wounds + CVEs.
  **HONEST-EDGE**: out of scope, documented.
- **Feature flags** — the one investigated category with NO framework gap. Skip.
- **Competitive frame**: Litestar (8.3k★) already advertises "batteries in core" as its whole
  pitch against FastAPI — validation that the batteries thesis wins converts. Django Ninja (9.1k★)
  = demand for FastAPI ergonomics + Django batteries. We are that thesis taken to the limit.

---

## 6. The build waves (proposal — Alok picks the order)

Each wave is a coherent, demo-able increment; conformance + bench gates run at every wave.

### Wave A — "Instant backend core" ⭐ recommended first
The category leap. `CREATE API FOR TABLE` (auto-CRUD with filter/sort/**pagination
envelope**/expand — absorbing bolt-on rank #5) · `CREATE POLICY` (build the existing spec) · auth
flows (JWT issuance, email/password, OAuth providers) · `CREATE SESSION` + CSRF (bolt-on rank #6,
the highest-👍 FastAPI issue ever) · admin dashboard served from the extension (bolt-on rank #7).
**Exit demo:** `LOAD quackapi; SELECT quackapi_serve('app.db')` → browse to /_admin, create a
table, instantly have a policied REST API. That demo IS the PocketBase adoption engine, pointed
at a warehouse.

### Wave B — "Public-internet trust"
TLS (spec exists) · WS main-port mount (spec exists) · multipart binary v2 + `CREATE STORAGE`
(local/S3, streaming — the UploadFile-spools-to-RAM killer) · body limits / header deadlines /
per-request timeouts / slowloris hardening (bolt-on rank #3 — wontfix'd across
uvicorn/starlette, and **we own the server, they don't**). Exit: safe to expose a real app.

### Wave C — "Ops batteries" (the Celery killer wave)
`CREATE CRON` + `CREATE JOB QUEUE` · `CREATE WEBHOOK` (svix-shaped: durable queue, backoff, HMAC
signing — rides the job-queue machinery) · `IDEMPOTENCY KEY` route clause · `CREATE HEALTH
CHECK` + `/metrics` (bolt-on ranks #2/#10 adjacent) · `CREATE RATE LIMIT` · `CREATE TOKEN
BLACKLIST` · response `CACHE` clause + ETag/304 · route versioning metadata · trace propagation ·
`CREATE TENANT` · `CREATE CONNECTION`. Exit: the report-05 summary table AND bolt-on ranks
2/3/6/10 answered in DDL.

### Wave D — "The differentiators" (nobody else can follow)
Analytics routes · lakehouse routes (Parquet/Iceberg/S3) · realtime `CREATE SUBSCRIPTION` over
SSE · FTS + vector search routes · bulk CSV/Parquet import/export endpoints · `CREATE AUDIT`
shadow tables + soft-delete semantics + time-travel reads (7×/12× Django-demand ratios) ·
HTML/HTMX polish (flash messages, Cache-Control statics, HTML-aware 422). Exit: the "why can't
10M people use this" demo — an app backend and its analytics dashboard from one file.

### Wave E — "Distribution & polish"
Typed TS SDK generation + CI OpenAPI validation gates · migrations auto-diff + backup one-liner
(bolt-on rank #4) · first-party test-harness packaging + docs (bolt-on rank #1 — mostly
structural for us already) · `quackapi init` scaffolding sugar · i18n-able 422 message table ·
docs/README "When NOT to use quackapi" · conformance 13-BUG grind · set-based batching (≥59k
gate) · HTTP/2 · Quack-protocol multi-writer probe. Exit: submittable to community-extensions
with receipts.

**Recommendation: A → B → C → D → E.** A is first because demo-ability — not performance, not
features — is what the adoption evidence says wins (PocketBase's HN launches, Supabase's growth).
We already have the performance receipts; what we lack is the 60-second "holy shit" moment. B
before C because auth without TLS is theater on a public host. D lands only after the boring
MUSTs so the differentiators read as "and it ALSO does this" rather than compensation.
