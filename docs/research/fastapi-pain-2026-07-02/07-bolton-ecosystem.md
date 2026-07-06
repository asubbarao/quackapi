# 07 — The Bolt-On Ecosystem: what production FastAPI apps install on top, and what users say the framework should do itself

**Date:** 2026-07-05 (research agent sweep; GitHub stars / PyPI last-month downloads pulled live
via GitHub API + pypistats.org; npm via api.npmjs.org. Reddit blocks crawlers, so community
evidence skews GitHub Discussions / HN / dev.to — triangulated where possible.)

**Scope:** the 20 categories NOT covered by reports 01–06 (which own auth/JWT, rate limiting,
CORS, background jobs, health checks, observability, middleware perf, multi-tenancy, deployment,
Pydantic churn, 422 shape, DTO boilerplate, structure chaos, lifespan, async starvation, GIL,
WS/SSE/TLS/multipart).

---

## 1. Pagination
- **Package**: `fastapi-pagination` (uriyyo) — **1,671★, 3.39M dl/mo** (one of the most-installed FastAPI add-ons, period). No real competitor.
- **Complaints**: Built-in pagination PR **rejected** by tiangolo (ORM-agnosticism rationale) — fastapi PR #2056. Recurring "how do I even paginate" threads: discussions #9456, #6589; cursor-pagination example request issue #4991. No standard envelope exists — every blog rolls its own `{items, total, page, size}` shape.
- **What it adds**: Generic `Page[T]` / `LimitOffsetPage[T]` / `CursorPage[T]` response models auto-documented in OpenAPI, `paginate()` integrations for ~15 ORMs, params dependency, first/last/next/prev links.

## 2. Response caching / ETag / conditional requests
- **Packages**: `fastapi-cache2` — 1,869★, **965K dl/mo, STALE** (no push ~12 mo, 108 open issues; spawned `fastapi-cache2-fork`). `cashews` — 585★, 578K dl/mo, active, ships `CacheEtagMiddleware`. `aiocache` 7.1M dl/mo (generic, no HTTP semantics). Dead `fastapi-etag` still pulls 26K/mo — demand without supply.
- **Complaints**: Discussion #7958 confirms **no built-in response caching** (FastAPI only caches dependencies within one request); tiangolo: DIY with aiocache. No ETag/Last-Modified/304 machinery anywhere in core (Django ships `ConditionalGetMiddleware` + `@etag`/`@condition`). fastapi-cache2 correctness bugs: #236 (Pydantic deserialization), #447, #491.
- **What they add**: `@cache(expire=…)` decorator, Redis/Memcached/in-memory backends, Cache-Control emission, ETag generation + If-None-Match → 304, key builders/namespaces/invalidation.

## 3. Admin panel (Django-admin envy)
- **Packages** (fragmented 4-way, none dominant): `sqladmin` **2,758★ / 833K dl/mo** (real usage leader, SQLAlchemy-only); `fastapi-admin` 3,802★ but only **19K dl/mo** (Tortoise-only, ~stale); `starlette-admin` 996★ / 206K dl/mo (ORM-agnostic); `fastapi-amis-admin` 1,559★ / 64K dl/mo. Combined ≈ ⅓ of fastapi-pagination alone.
- **Complaints**: Discussion #13430 "admin panel similar to Django Admin" (2025) — answer is a per-ORM decision tree, nobody has stability data. HN "Django vs FastAPI 2025" (id 43557087): *"getting an admin panel literally for free is difficult to beat."* Build-it-twice comparison: 30 min to admin in Django vs **2–3 days** in FastAPI (medium.com/engineering-playbook). Every package README self-describes as "inspired by Django admin."
- **What they add**: auto CRUD UI from ORM models, search/filter/sort, FK/M2M widgets, exports, auth backends, bulk actions. None replicate Django admin's permissions/inlines/15-yr polish.

## 4. Database migrations
- **Package**: Alembic — 4,237★, **188M dl/mo**, only game in town. FastAPI/SQLModel ship zero migration story.
- **Complaints**: autogenerate can't detect renames (emits DROP+CREATE — data loss); `server_default` compare bugs (alembic #1177, #1454, discussion #1204); **multiple-heads** team pain ("breaks migrations on main multiple times a week" — Alan eng blog); async env.py setup pain (silent empty migrations). SQLModel friction: #85, #289 (`EmailStr` field → autogenerate exits 1 with no traceback), #466, #9. Tiangolo's SQLModel roadmap (#654) lists "integrated migrations" as unshipped future work — an acknowledged `makemigrations` gap.
- **What's missing vs Django**: rename prompts, automatic linear ordering/merge prompts, zero-config wiring — every project re-derives ~100 lines of env.py.

## 5. API versioning
- **Packages**: `fastapi-versioning` — 846★, 81K dl/mo, **abandoned since Aug 2021** (most-starred = the dead one); `fastapi-versionizer` (fork-because-abandoned, 106★); `cadwyn` (Stripe-style migration-based versioning, 303★, active; its 14.9M dl/mo is CI-inflated — trust stars).
- **Complaints**: Discussion #8177 open since **2019** — tiangolo's answer is "mount routers with different prefixes" (manual duplication); sub-apps split the docs pages. fastapi-versioning #73 "Maintained Fork Inquiry". Media-type/header versioning request #4694 unimplemented. DRF has `versioning_class` in core; FastAPI has nothing.
- **What they add**: `@version(m,n)` decorators, inherited-and-overridden version trees, per-version OpenAPI docs, Cadwyn's one-latest-implementation + version-change migration classes with header (date) selection.

## 6. Outbound webhooks (retry/signing)
- **Packages**: `svix` (server 3,279★; client 7.0M dl/mo — a **VC-backed company exists because of this gap**); `standard-webhooks` spec (1,700★; lib 2.2M dl/mo; adopters: OpenAI, Anthropic, Twilio, PagerDuty, Supabase…).
- **Complaints**: FastAPI's `app.webhooks` is **documentation-only** — docs literally say "the code to actually send those requests is up to you." Svix Launch HN (id 27528202): Fly.io's mrkurt — *"retry handling, notifications when they break (but not the first time…)… is not simple"*; multiple "I built my own and wasted months" testimonies.
- **What they add**: durable delivery queue, exponential-backoff retries, HMAC-SHA256 signing + rotation + replay protection, per-customer endpoint CRUD/health/auto-disable, delivery-log portal with replay.

## 7. Idempotency keys
- **Packages**: none won. `asgi-idempotency-header` (snok) — **25★**, 28K dl/mo, dead since 2022; `idemptx` 23★; `fastapi-idempotent` 152 dl/mo. Nothing cracked 30★ in 5+ years — **that is the data: everyone hand-rolls Redis `SET NX EX`**.
- **Complaints**: Discussion #3555 "Best practices for Idempotent Requests" active **2021→2026**, with hand-rolled guidance still being posted in 2026 (atomic Redis lock, 409 on body mismatch, TTL'd response replay); recommended libs flagged in-thread as outdated. DIY blog genre thriving (OneUptime Jan-2026, Zuplo guide).
- **What core lacks**: any `Idempotency-Key` convention, middleware, or docs guidance at all.

## 8. Sessions / CSRF
- **Packages**: Starlette `SessionMiddleware` (signed-cookie only, via itsdangerous); `starsessions` 122★ / 446K dl/mo (server-side stores); `fastapi-csrf-protect` 110★ / 126K dl/mo; 4+ tiny competing session micro-libs (fragmentation signal).
- **Complaints**: **"First-class session support" is the highest-👍 feature issue in FastAPI history (75👍, #754, closed unresolved)**. CSRF built-in request #4419/discussion #8547 declined ("fastapi_csrf_protect… looks strange" — the asker). HN Jan-2026 (id 46822393): *"moved the project to Django-Ninja and development accelerated massively… storing the session in the database [had] only half finished solutions"* in FastAPI. Litestar ships CSRF in core and users cite it (HN id 36658988).
- **What core lacks**: server-side session backends, revocation/logout-that-works, session-ID regeneration, CSRF middleware (Django ships all by default). Cookie-auth/SSR users get no framework guidance.

## 9. Metrics endpoint
- **Package**: `prometheus-fastapi-instrumentator` — 1,471★, **14.1M dl/mo** (plus starlette-exporter 1.76M, starlette-prometheus 240K — ~16M/mo combined; near 1:1 with production deploys).
- **Complaints**: low-noise but universal — every prod tutorial's step 1; FastAPI maintainer Kludex maintains his own glue repo; Grafana's official FastAPI dashboard (ID 16110) presumes third-party instrumentation; discussion #4857 (separate-port metrics); breakage when the framework moves (instrumentator #80). Related core ask: **built-in K8s readiness/liveness probes, 62👍 (#1907), still unshipped**.
- **What it adds**: `/metrics` exposition, RED metrics with route-template labels (cardinality-safe), status-class grouping, multiprocess/gunicorn mode, exclude-handlers. FastAPI ships literally nothing.

## 10. Feature flags
- **Verdict: NOT a framework gap.** LaunchDarkly SDK 10.7M dl/mo, growthbook 3.1M, Unleash 2.0M, openfeature 1.5M, flagsmith 1.2M — vs the lone FastAPI-native `fastapi-featureflags` at 153★ / 7.7K dl/mo, dormant. No GitHub discussion requesting built-in flags surfaced. The only glue needed is a lifespan singleton + `Depends()` (~30 lines). Deprioritize.

## 11. i18n of errors/responses
- **Packages**: `pydantic-i18n` 101★ / 38K dl/mo; `fastapi-babel` 69★ / 29K; `starlette-babel` 23★. Tiny — most teams stay English-only or translate client-side.
- **Complaints**: pydantic #322 (open since **2018**) — error strings hardcoded English in Rust pydantic-core; only post-hoc string-matching is possible, and every pydantic upgrade can break the match table. FastAPI discussion #8060: tiangolo — "FastAPI doesn't generate any text strings itself"; user pushback — *"Excluding this feature forces users to hack their way into it."* Django ships pre-translated validation messages in ~100 locales in core.
- **What they add**: locale-negotiation middleware, gettext workflow, 422-payload rewriting via custom `RequestValidationError` handler keyed on Accept-Language.

## 12. Static files + templates / full-stack & HTMX story
- **Packages**: `fasthx` 723★; `fastapi-htmx` 329★; and the defection product — **FastHTML (AnswerDotAI) 6,960★ / 809K dl/mo**, launched via an 867-point HN thread (id 41104305) explicitly as a reaction.
- **Complaints**: no flash messages (#5796 — hand-roll Flask's `flash()`); `StaticFiles` can't set Cache-Control (#1433, #7618), no compression/manifest cache-busting, GZip+304 bug (#4050), no WhiteNoise equivalent; form models bypassed Pydantic for years (discussion #5951, partially fixed only in 0.113, Sep-2024); a 422 JSON body is useless for server-rendered form re-display; discussion #11966 "take inspiration from FastHTML" unanswered. HN (harel): *"reworking a fastapi project to Django+ninja because it simply grew in scope."*
- **What ecosystem adds**: decorator-based SSR, HTMX partial-vs-full-page handling, same-endpoint HTML/JSON content negotiation.

## 13. CLI scaffolding / project generation
- **Tools**: `full-stack-fastapi-template` **44,057★** (the de facto answer — a clone-me repo, not a CLI); `fastapi-cli` (just a uvicorn launcher — 53M dl/mo only as a `fastapi[standard]` dep); **`fastapi-best-practices` README has 17,635★** — a conventions document out-starring every actual generator combined = the clearest "structure chaos" signal. The two `startproject` attempts are dead (manage-fastapi, last push 2024; fastapi-mvc, 145 dl/mo, cookiecutter archived).
- **Complaints**: discussion #11525 revolt over fastapi-cli's forced deps (reverted in 0.112.0); "a great feature missing in frameworks like FastAPI" re Django `startproject` (Marc Nealer); "beyond Hello World you'll need to make many decisions and implement things yourself" (Blueshoe).
- **What's missing**: `rails new` / `django-admin startproject` / resource generators; skeleton + auth + Alembic + tests + Docker pre-wired only via copying a 44k-star opinionated template.

## 14. Body size limits / timeouts / slowloris — the security gap
- **Workarounds**: `content-size-limit-asgi` (36★ yet 25K dl/mo, barely maintained — desperation signal); the copy-pasted 413 middleware from #362/#8167; `asyncio.wait_for(call_next…)` timeout middleware from discussion #7364 (which **leaks — handler keeps running after the 504**); and mostly: nginx/traefik `client_max_body_size`, i.e., protection lives outside Python.
- **Complaints (verified issue numbers)**: uvicorn **#157** (gunicorn-style request-start/complete timeouts + line/header size limits) **closed not-planned**; uvicorn #95, #1276 (per-request timeout — "uWSGI had this option") closed unimplemented; starlette **#890**: *"a malicious user could send a 30GB JSON and cause OOM… Django, Quart support this"* — never added globally; fastapi #362/#1181 answered "use nginx"; slow-header clients hold connections (only knob is misnamed `--h11-max-incomplete-event-size`, uvicorn discussion #1726). The form-limits patch that finally shipped was itself broken: **CVE-2026-54283** (limits silently ignored for urlencoded bodies; fixed Starlette 1.3.1). Uvicorn docs' official posture: put nginx/CDN in front for "buffering slow requests" and "serious DDOS protection."
- **What's missing**: 413 on Content-Length AND streamed-byte counting (chunked bodies), header-read deadlines, request-line/header caps, per-request wall-clock timeout with cancellation.

## 15. Audit logging / CDC on models
- **Packages**: `SQLAlchemy-Continuum` 643★ / **606K dl/mo** (rescued into a community org after years of stagnation); `sqlalchemy-history` (fork-because-unmaintained, 69K dl/mo); `postgresql-audit` 35K dl/mo. Django contrast: **django-simple-history 2,453★ / 4.16M dl/mo — ~7×**.
- **Complaints**: Continuum **#276 AsyncSession support still open** — incompatible with the canonical FastAPI async stack; postgresql-audit #32 users shopping between the same author's two half-maintained libs; SQLAlchemy maintainers punt versioning to example recipes.
- **What they add**: auto shadow `_version` tables, transaction grouping, revert, history queries; actor attribution from request → DB record (django-simple-history does this via middleware out of box; FastAPI users wire contextvars themselves).

## 16. Soft deletes / row versioning
- **Packages**: `sqlalchemy-easy-softdelete` — **73★ carrying 120K dl/mo** (vacuum signal). Django: django-safedelete 709★ / **1.49M dl/mo — ~12×**.
- **Complaints**: sqlalchemy discussion #11468 — Mike Bayer's on-record refusal ("not appropriate as a built-in… supporting any and all edge cases"), noting the same ask was filed at least 4× (#3596, #4004, #7973); reached FastAPI org via sqlmodel discussion #989. Everyone copies the ~40-line `with_loader_criteria` + `do_orm_execute` recipe and rediscovers its failure modes (lazy-loads, `session.get()`, unique constraints vs dead rows) per team. Optimistic locking exists in SQLAlchemy (`version_id_col`) but nothing in FastAPI surfaces 409-conflict handling.

## 17. GraphQL
- **Packages**: `strawberry-graphql` 4,684★ / 7.3M dl/mo (docs-blessed); ariadne 2,343★ / 1.3M. Starlette's built-in GraphQLApp was **removed in 0.17** ("not much interest in maintaining it," starlette #1135/#619) — FastAPI has had zero built-in GraphQL since.
- **Complaints**: **can't use `Depends()` in resolvers** — strawberry #2413 (open since 2022) + #1357; the `context_getter` workaround "sacrifices type hints." Per-request dataloader wiring is boilerplate folklore (strawberry discussion #1863); subscription protocol confusion (#1640, #1623) plus security fallout (GHSA-vpwc-v33q-mq89 auth bypass; CVE-2026-35526 WS DoS).

## 18. Testing fixtures / factories — the "every project reinvents conftest.py" tax
- **Packages**: pytest-asyncio 218M dl/mo; factory_boy 22.6M; polyfactory 9.5M (Litestar-org — auto-generates data from Pydantic type hints); testcontainers 25.9M. Baseline: **pytest-django 21M dl/mo** shows what a first-party harness looks like.
- **Complaints**: pytest-asyncio **0.21→0.23 breakage** (#706, #670, #991) → "Future attached to a different loop" plague in FastAPI+asyncpg suites (fastapi #5692/#8415); teams pinned 0.21 for a year. httpx 0.27 deprecated `app=` → `ASGITransport` churn broke FastAPI's own docs (fixed in PR #12084). The transaction-rollback-per-test recipe (connection + outer txn + SAVEPOINT + `after_transaction_end` listener) is ~80 lines of hand-rolled folklore (fastapi #4507; oddbird.net/2024/02/09/testing-fastapi). `dependency_overrides` is the only tool provided.
- **What's missing vs Django**: transactional test case, test-DB lifecycle, client fixture, settings override, factory story — despite FastAPI being Pydantic-native (polyfactory proves the factory could be nearly free).

## 19. OpenAPI client SDK generation
- **Packages**: @hey-api/openapi-ts 5,052★ / 14.0M npm dl/mo; orval 6,202★ / 6.4M; openapi-python-client 1,966★ / 2.8M PyPI. FastAPI's own docs page delegates to them.
- **Complaints**: **FastAPI's docs ship a workaround for their own defaults** — ugly operationIds (`createItemItemsPost`) require `generate_unique_id_function` + a second post-processing script; auto-generated `Body_login_login_access_token_…` schema names become SDK class names with no rename hook (discussion #7448); first-party Pydantic-native client asked for and declined (discussion #8242, 2019→2024); OpenAPI 3.1 output broke older generators.

## 20. File / object storage
- **Packages**: raw boto3 (3.4B dl/mo) / aioboto3 (36M) win by default; the Django-storages-equivalents are rounding error — `fastapi-storages` 111★ / 34K dl/mo, `sqlalchemy-file` 115★ / 57K. **No abstraction has won; everyone hand-rolls S3 glue.**
- **Complaints**: `UploadFile` spools everything to RAM/tmp **before the handler runs** — fastapi #5413/#6120 (4×20MB uploads: ~13s FastAPI vs 0.6s aiohttp), #9828, #14374; tuning knobs buried in python-multipart (#201); upload size limit was a years-long ask (#14468). Streaming multipart→S3 requires bypassing UploadFile entirely via `streaming-form-data` — the canonical recipe is **a gist**, not docs. Community consensus workaround: presigned URLs, i.e., don't upload through FastAPI at all.

---

## Discovered categories not on the original list
- **DI scopes/lifetimes** — no singleton/app scope (discussion #9215; fastapi-injector ecosystem; roadmap item "improved dependency overrides"); Litestar's layered `Provide()` DI is counter-positioning.
- **Route architecture / circular imports / class-based views** — b-list.org Aug-2025 Litestar post; issue #270 (39👍).
- **ORM-derived schemas / triple-model duplication beyond DTOs** — issue #214 (83👍); SQLModel stagnation; Litestar SQLAlchemy DTOs as the marketed fix.
- **Signals/events system** — #4127; `async-signals` ("missed Django signals… copied the library from Django").
- **Task scheduling (cron, distinct from background jobs)** — fastapi-scheduler etc.
- **RFC 9457 problem-details errors** — unchecked roadmap item (#10370).
- **Reference docs (not tutorials)** — #804 (63👍); cited in 2025 Litestar-migration wave.
- **Governance/bus-factor as a meta-complaint** — "Find maintainers" #4263 (87👍); "Frustrated of FastAPI slow development" discussion #3970 (88 upvotes); HN id 29471609: *"you re-implemented django yet again."*
- **Competitive positioning data**: Litestar (8,318★ / 1.76M dl/mo) advertises in-core: JWT auth+guards, rate limiting, response caching, Prometheus/OTel, Channels, DTOs, **built-in pagination**, CSRF, HTMX, msgspec serialization. Django Ninja (9,114★ / 2.75M dl/mo) = demand for FastAPI ergonomics inside Django's batteries; its Motivation page names FastAPI's `Depends()` verbosity and ORM-bridge failures directly. (FastAPI itself: 100,050★ / ~474M dl/mo.)

---

## Ranked top 10 by (complaint frequency × breadth of affected users)

| # | Category | Why it ranks here |
|---|---|---|
| 1 | **Testing harness** (18) | Affects literally every project; pytest-asyncio 0.23 + httpx ASGITransport churn broke everyone simultaneously; the rollback-per-test recipe is universal hand-rolled folklore; pytest-django (21M dl/mo) proves the demand shape. |
| 2 | **Metrics /metrics endpoint** (9) | 14M dl/mo on a ~solo-maintained package ≈ every production deploy; plus 62👍 unshipped K8s-probes issue. Low complaint noise only because installing it is reflexive. |
| 3 | **Body limits / timeouts / slowloris** (14) | Universal production security exposure; multiple closed-wontfix issues across uvicorn/starlette/fastapi; a CVE in the partial fix; official answer is "use nginx." |
| 4 | **Migrations story** (4) | Every SQL-backed app; rename/default/multiple-heads/async-env.py pain is constant; tiangolo's own roadmap admits the gap (sqlmodel #654). |
| 5 | **Pagination** (1) | 3.4M dl/mo single package; core PR explicitly declined; no standard envelope — every public API re-decides. |
| 6 | **Sessions / CSRF** (8) | Sessions = highest-👍 feature issue ever (75👍); CSRF declined; direct 2026 evidence of users defecting to Django-Ninja/Litestar over it. Breadth limited to cookie-auth/SSR — but that's a large silent cohort. |
| 7 | **Admin panel** (3) | The loudest single "what I miss from Django" item on HN 2023–2025; 4-way fragmented ecosystem where the most-starred option is stale; 30-min-vs-3-days quantified. |
| 8 | **Project scaffolding / structure** (13) | Every new project pays it; a conventions README (17.6k★) out-stars all generators; the only real answer is cloning a 44k★ template; both CLI attempts died. |
| 9 | **SDK generation workflow** (19) | Everyone with a frontend; FastAPI's docs ship workarounds for its own operationId/`Body_*` naming; first-party client declined. |
| 10 | **Response caching / ETag** (2) | ~1M dl/mo riding a package stale for a year with 108 open issues + two forks; no conditional-request support in core vs Django's built-ins. |

Just off the list: **full-stack/HTMX story** (spawned an entire competing framework, FastHTML 7k★, but its users partially self-select out of FastAPI), **outbound webhooks** (gap big enough for a YC company + industry spec, but only a minority of apps send webhooks), **API versioning** (abandoned 80K-dl/mo leader — high pain, public-API subset), **file storage** (universal UploadFile perf complaints but presigned-URLs bypass caps the pain), **DI scopes** (rising, powers the Litestar migration narrative). **Feature flags** is the one investigated category with no framework gap.

**Cross-cutting patterns for roadmap use**: (a) the maintainers' consistent answer is "recipe/middleware/nginx/third-party" — the vacuum is then filled by micro-packages with 25–150★ carrying 25K–1M dl/mo, frequently abandoned (fastapi-versioning, asgi-idempotency-header, fastapi-cache2, content-size-limit-asgi) — **abandonment of load-bearing plugins is itself the top-recurring complaint**; (b) Django-analog download ratios quantify each gap directly (simple-history 7×, safedelete 12×); (c) meta-complaints (governance #4263 87👍, tutorial-only docs #804 63👍) now outrank any single feature request and are fueling the 2025-26 Litestar/Django-Ninja migration wave.
