# quackapi — Feature Ledger (authoritative)

**Descriptor target:** 0.2.0 (release candidate; not a publication receipt)
**Source of truth:** this checkout's source tree and versioned tests; cited report paths are historical evidence only.
**Rule:** implemented claims below are backed by source or tests. Design studies and external packaging notes remain explicitly labelled.
**Publication:** this ledger is not a release receipt. Community packages follow the separate registry pin and signed build pipeline; see [migration notes](MIGRATION.md).
**Current validation:** [PR #58 CI](https://github.com/asubbarao/quackapi/actions/runs/40426089540) passed all five platform builds and 63 SQL suites with zero skips at application head `ee3034ca`. Historical 89-case and corpus figures below describe earlier experiments; the reviewed candidate also passed 93 conformance cases.

**Evidence index**

| Artifact | What it proves |
|----------|----------------|
| `rg 'CREATE …' src/` + registered functions in `src/` | 8 CREATE nouns plus the lifecycle, request, auth, queue, HTTP, GraphQL, middleware, policy, stream, and bridge functions |
| `ls test/sql test/http test/conformance` | Versioned tests for each surface |
| `bash test/conformance/run.sh` → `/tmp/quackapi_conformance_rerun/` | **Live** FastAPI harness: **89/89 PASS (100%)** |
| `/tmp/quackapi_fastapi_eq/SCORECARD.md` | **Stale** baseline: **62/89 (69.7%)** pre body/form/multipart/cookies/headers/redirect/openapi/redoc |
| `/tmp/quackapi_corpus/{PYTHON,RUBY,GO,NODE,SPEC}.md` + IR parquets | 1576 routes · 4204 model fields · 36 repos |
| `/tmp/quackapi_{fromfast,rails_bridge,pydantic_bridge,handler_bridge}.md` | Bridge status + fidelity |
| `/tmp/quackapi_spec_*/SPEC.md` (20) | DESIGNED-not-built feasibility |
| `/tmp/quackapi_wanted/BACKLOG.md`, `/tmp/quackapi_next15/BACKLOG2.md` | Roadmap inputs |
| `/tmp/quackapi_{arrow,scope9,scope_free}.md` | Ext composition / free-win scope |

---

## 1. Implemented in this source tree

### 1.1 Eight CREATE nouns (parser extensions)

| # | CREATE noun | Implementation | Inspect / apply | Versioned tests | FastAPI equivalent |
|---|-------------|----------------|-----------------|-----------------|--------------------|
| 1 | **`CREATE [OR REPLACE] ROUTE`** | `src/quackapi_ddl.cpp` | `quackapi_routes()` | `test/sql/quackapi_routes.test`, `test/http/{routing,validation,body,body_schema,form,multipart,headers,cookies,redirect,trailing_slash}.test.sh` | `@app.get/post/…`, path/query/header/cookie/body params, status codes |
| 2 | **`CREATE [OR REPLACE] AUTH`** | `src/quackapi_auth.cpp` | `quackapi_auths()`, `quackapi_add_api_key()`, `quackapi_verify_auth()` | `test/sql/quackapi_auth.test`, `test/http/auth.test.sh` | `APIKeyHeader` / `HTTPBearer` + `Depends` |
| 3 | **`CREATE [OR REPLACE] [API] GROUP`** | `src/quackapi_ddl.cpp` | `quackapi_groups()` | `test/sql/quackapi_group.test`, `test/http/group.test.sh` | `APIRouter(prefix=…, tags=…, dependencies=…)` |
| 4 | **`CREATE [OR REPLACE] API FOR TABLE`** | `src/quackapi_table_api.cpp` | expands to routes → `quackapi_routes()` | `test/sql/quackapi_table_api.test` | FastAPI + SQLAlchemy list/get scaffold |
| 5 | **`CREATE [OR REPLACE] QUEUE`** | `src/quackapi_queue.cpp` | `quackapi_queues()`, `quackapi_enqueue/dequeue/ack/nack`, table `quackapi_jobs` | `test/sql/quackapi_queue.test`, `test/http/queue.test.sh` | BackgroundTasks / Celery-shaped durable jobs (DB-native) |
| 6 | **`CREATE [OR REPLACE] STREAM`** | `src/quackapi_stream.cpp` | `quackapi_streams()` | `test/sql/quackapi_stream.test`, `test/http/stream.test.sh` | SSE (`EventSourceResponse`); WebSocket uses `CREATE STREAM … WS` |
| 7 | **`CREATE [OR REPLACE] ROW ACCESS POLICY`** | `src/quackapi_policy.cpp` | `quackapi_policies()` | `test/sql/quackapi_policy.test`, `test/http/policy.test.sh` | No first-class FastAPI primitive (row filters / RLS) |
| 8 | **`CREATE [OR REPLACE] MASKING POLICY`** | `src/quackapi_policy.cpp` | `quackapi_policies()` | same | No first-class FastAPI primitive (column mask) |

DROP forms exist for ROUTE, AUTH, GROUP/API GROUP, QUEUE, STREAM, ROW ACCESS POLICY, MASKING POLICY (`rg 'DROP ' src/`).

### 1.2 Route surface features (on `CREATE ROUTE`)

| Feature | DDL / function | Versioned test | FastAPI equivalent |
|---------|----------------|----------------|--------------------|
| Methods GET/POST/PUT/PATCH/DELETE/**HEAD** | `CREATE ROUTE … <METHOD>` | `quackapi_routes.test`, conformance `methods/*` | `@app.get` … `@app.head` |
| Path params `:id` / `{id}` + `$id` bind | pattern + prepare | `validation.test.sh`, conformance `params/*` | Path parameters |
| **PARAM** typed query + defaults + **GE/GT/LE/LT/MIN_LENGTH/MAX_LENGTH** | `PARAM name TYPE [DEFAULT …] [GE…]` | `validation.test.sh`, conformance `search_limit_*` | `Query(ge=…, le=…)` |
| **PARAM … HEADER / COOKIE** [wire-name] | `PARAM x HEADER` / `COOKIE` | `headers.test.sh`, `cookies.test.sh`, cases `header_param`/`cookie_param` | `Header()`, `Cookie()` |
| **JSON body** field bind + wrong CT / malformed JSON → 422 | body binder in `quackapi_server.cpp` | `body.test.sh`, cases `post_users_json_body*` | Pydantic body model |
| **BODY SCHEMA** JSON Schema validation (`json_schema` ext) | `BODY SCHEMA '<json>'` | `body_schema.test.sh` | Pydantic model / `json_schema_extra` |
| **BODY TYPE** native DuckDB JSON transform validation | `BODY TYPE '<structure>'` | body-type SQL/HTTP tests | typed request body model |
| **Form** `application/x-www-form-urlencoded` | body binder | `form.test.sh`, case `form_submit` | `Form()` |
| **Multipart** fields + file (`$file`, `$filename`) | body binder | `multipart.test.sh`, case `multipart_upload` | `File()`, `UploadFile` |
| **STATUS n** | `STATUS 201` etc. | conformance `status_*` | `status_code=` / `Response` |
| **TIMEOUT / WITH (timeout_sec)** | per-request httplib read/write deadline (default = serve 30s) | `quackapi_route_timeout.test` | no first-class FastAPI equivalent (uvicorn/gunicorn worker timeouts are process-global) |
| **RATE LIMIT** | `RATE LIMIT n PER s [BY ip\|token\|key]` → 429 + `Retry-After` | `test/sql/quackapi_rate_limit.test` | application limiter / middleware |
| **Redirect** (3xx + `location` column) | `STATUS 307 AS SELECT '…' AS location` | `redirect.test.sh`, case `redirect_307` | `RedirectResponse` |
| **Set-Cookie** response | `AS set_cookie` column | `redirect.test.sh` / case `set_cookie` | `Response.set_cookie` |
| **html / text** content types | single column named `html`/`text` | conformance `ct_html`/`ct_text` | `HTMLResponse` / `PlainTextResponse` |
| **REQUIRE &lt;auth&gt;** | clause on ROUTE / inherited from GROUP | `auth.test.sh` | `dependencies=[Depends(…)]` |
| **GROUP &lt;g&gt;** membership | `GROUP` / `IN GROUP` on ROUTE | `group.test.sh` | `include_router` |
| Strict int bind (reject `1.5`, `1e2`) | `BindParamValue` digit check | cases `get_user_bad_float`, `search_limit_1e2` | Pydantic v2 int (parity; was BUG on stale card) |
| 422 FastAPI-shaped `{detail:[{loc,msg,type}]}` | `ValidationErrorJson*` | case `422_shape_keys` | `RequestValidationError` |
| **405 + Allow** | method scan in `HandleRequest` | cases `health_post_405`, `allow_header_on_405` | Starlette 405 + `Allow` |
| Auto **HEAD** for GET | server method registration | cases `health_head_auto`, `get_user_head_explicit` | Starlette auto-HEAD |
| **OPTIONS** 405 without CORS; **204** preflight with CORS | CORS branch | case `health_options`, `cors.test.sh` | FastAPI without/with `CORSMiddleware` |
| Trailing-slash **307** (Starlette-style) | path alternate | cases `list_users_trailing_slash`, `health_trailing_slash` | Starlette `redirect_slashes` |

### 1.3 Server, OpenAPI, CORS, queue, stream, policy

| Feature | DDL / function | Versioned test | FastAPI equivalent |
|---------|----------------|----------------|--------------------|
| Serve / stop / inspect | `quackapi_serve` with lifecycle, health, access-log, compression, transport, client, upload, cancellation, and resource-limit options; `quackapi_wait`, `quackapi_stop`, `quackapi_servers` | `quackapi_serve_block.test`, resource-limit and HTTP tests | `uvicorn` / `app` process |
| **CORS** | `cors_origins` arg / `SET quackapi_cors_origins` | `test/sql/quackapi_cors.test`, `test/http/cors.test.sh` | `CORSMiddleware` |
| **static_dir** unrouted GETs | `quackapi_serve(…, static_dir := …)` | description.yml / README | `StaticFiles` (mount; prefix sugar still SPEC) |
| **Health routes** | built-in `/health` and `/healthz` when `health_routes := true` | health and HTTP tests | liveness/readiness probes |
| **Response compression** | `compression := 'auto'\|'gzip'\|'zstd'\|'off'`, `compression_min_bytes` | `quackapi_compression.test`, `test/ultra/test_compression.sh` | gzip/zstd middleware |
| **Access log** | stderr JSON or `access_log := '<table>'` with async batching and `/healthz` overflow count | `quackapi_access_log.test` | structured access logging |
| **OpenAPI 3.1** | built-in `GET /openapi.json` (`quackapi_openapi.cpp`) | `test/sql/quackapi_openapi.test`, case `openapi_json` | auto OpenAPI |
| **Swagger UI** | `GET /docs` | same + case `docs_get` | `/docs` |
| **ReDoc** | `GET /redoc` | `test/http/redoc.test.sh`, case `redoc_get` | `/redoc` |
| **Queue** enqueue/dequeue/ack/nack | `quackapi_enqueue` / `_dequeue` / `_ack` / `_nack`, `quackapi_jobs` | queue SQL + HTTP tests | BackgroundTasks + broker |
| **SSE stream** | `CREATE STREAM … GET '/path' AS <select>` | stream SQL + HTTP | `StreamingResponse` / SSE |
| **WebSocket** | **built** — `CREATE STREAM … WS` (push rows, or reply per `$message`) | `quackapi_stream.test`, `test/cpp/quackapi_websocket_test.cpp` | `WebSocket` |
| **Row access + masking policies** | CREATE … POLICY + ALTER TABLE bind | policy SQL + HTTP | — (DB-native leapfrog) |
| Quack RPC authentication | Native core Quack hooks/defaults; no REST auto-wiring | `quackapi_quack_bridge.test` | DuckDB core Quack |
| Security / key hashing | auth + security tests | `quackapi_security.test` | secret storage patterns |

### 1.4 Registered SQL functions (live `duckdb_functions()`)

```
quackapi_ack, quackapi_add_api_key, quackapi_auths, quackapi_dequeue, quackapi_enqueue, quackapi_exec, quackapi_graphql_routes,
quackapi_graphql_tables, quackapi_groups, quackapi_http_pool,
quackapi_middlewares, quackapi_nack, quackapi_policies,
quackapi_post, quackapi_queues, quackapi_renew, quackapi_request,
quackapi_routes, quackapi_serve, quackapi_servers, quackapi_stop, quackapi_streams,
quackapi_verify_auth, quackapi_wait, quackapi_requests,
quackapi_telemetry_status, quackapi_telemetry_flush,
quack_from_routes, quack_from_models
```

Plus durable table **`quackapi_jobs`** (queue) created on first `CREATE QUEUE`.

---

## 2. FASTAPI COMPARISON (refreshed)

### 2.1 Headline — old vs new

| Metric | **STALE** `/tmp/quackapi_fastapi_eq/SCORECARD.md` | **REFRESHED** `test/conformance/run.sh` (this session) |
|--------|----------------------------------------------------:|--------------------------------------------------------:|
| Overall | **62 / 89 (69.7%)** | **89 / 89 (100.0%)** |
| PASS / FAIL / N/A | 62 / 17 / 10 | **89 / 0 / 0** |
| BUG | 9 | **0** |
| NOT-BUILT-YET | 16 | **0** |
| STRONGER class rows | ≥2 noted | **1** (`response_model_exclude`) + more behaviors below |
| Harness port | archive under `/tmp/quackapi_fastapi_eq/` | **versioned** `test/conformance/` on main |

**Re-run command (evidence):**

```bash
PORT=18791 RESULTS_DIR=/tmp/quackapi_conformance_rerun \
  bash /Users/aloksubbarao/personal/quackapi/test/conformance/run.sh
# → wrote /tmp/quackapi_conformance_rerun/{results.jsonl,summary.json}
# python3 test/conformance/render_scorecard.py → overall 89/89 (100.0%)
```

Also documented in-repo: `docs/FASTAPI_PARITY.md` (G4 final: 89/89).

### 2.2 Per-group (refreshed)

| Group | Old (stale scorecard) | New (re-run) |
|-------|----------------------:|-------------:|
| auth | 6/6 (100%) | **6/6 (100%)** |
| params | 22/26 (85%) | **26/26 (100%)** |
| methods | 7/9 (78%) | **9/9 (100%)** |
| validation | 13/21 (62%) | **21/21 (100%)** |
| status_codes | 3/5 (60%) | **5/5 (100%)** |
| routing | 7/12 (58%) | **12/12 (100%)** |
| content_types | 4/7 (57%) | **7/7 (100%)** |
| openapi | 0/3 (0%) | **3/3 (100%)** |

### 2.3 What closed the gap (stale → now)

Previously **NOT-BUILT-YET / BUG** on the stale card — now **PASS** on main:

| Surface | Stale | Now (case ids) |
|---------|-------|----------------|
| JSON body / malformed / wrong CT | N/A | `post_users_json_body`, `post_users_malformed_json`, `post_users_wrong_ct` |
| Form / multipart | N/A | `form_submit`, `multipart_upload` |
| Header / Cookie params | N/A | `header_param`, `cookie_param` |
| Redirect / Set-Cookie | N/A | `redirect_307`, `set_cookie` |
| OpenAPI + docs + redoc | N/A | `openapi_json`, `docs_get`, `redoc_get` |
| Optional query defaults | FAIL | `search_limit_missing` |
| Query `le` constraint | FAIL | `search_limit_le` |
| Strict int (`1.5`, `1e2`) | BUG 200 | `get_user_bad_float`, `search_limit_1e2`, `post_users_age_float_str` → 422 |
| `LIMIT -1` 500 | BUG | `search_limit_neg` → 200 `[]` |
| 405 missing `Allow` | BUG | `allow_header_on_405` |
| OPTIONS without CORS | FAIL/N/A | `health_options` → 405 + Allow |
| Auto-HEAD | partial | `health_head_auto` |
| Trailing slash | quirk 200 | 307 (Starlette match) |

### 2.4 STRONGER than FastAPI

| Behavior | Evidence |
|----------|----------|
| Response JSON is **DB-typed** (bool/number/null preserved) | cases `ct_json`, `json_null_bool_types` |
| **int64 overflow** path → **422** (fail closed) | `get_user_overflow` **STRONGER** |
| Handlers are **set-based SQL** (validation + projection one prepare) | all routes; scorecard notes |
| Custom responses via **column names** (`html`, `text`, `location`, `set_cookie`) | content_types + redirect cases |
| **SELECT list = response model** (no dual Pydantic layer) | case `response_model_exclude` class=STRONGER |
| Strict integer bind (no silent cast-round) | validation group 100% |
| **Policies / queue / SSE** as first-class DDL | no FastAPI core peers |

### 2.5 Honest remaining gaps (outside the 89-case harness)

These are **not** counted against the 100% harness score; they are product/roadmap gaps vs the full FastAPI ecosystem:

| Gap | Notes | Cite |
|-----|-------|------|
| **OIDC / OAuth2 SSO** browser code flow | JWT/API_KEY only; compose `quack_oauth` / `jwt` for resource-server patterns | `/tmp/quackapi_spec_oidc/SPEC.md` |
| **Signed cookie sessions + CSRF** | not built | `/tmp/quackapi_spec_sessions/SPEC.md` |
| **Middleware BEFORE/AFTER SQL** | **shipped** — `CREATE MIDDLEWARE` around ordinary routes and `quackapi_request` | `docs/MIDDLEWARE.md`, middleware tests |
| **Response gzip/zstd** | **shipped** — Accept-Encoding negotiate; `compression` serve knobs | `quackapi_server.cpp` MaybeCompress |
| **FORMAT / Accept** | **shipped** json/ndjson/csv/**parquet**/**arrow** (IPC stream via community nanoarrow) | format tests; Accept stream/file |
| **GraphQL thin v0** | **shipped** — dual surface (REST + GQL → SQL); allowlist + **`CREATE GRAPHQL ROUTE`** | `quackapi_graphql.cpp`, `graphql-v0.md` |
| **In-process TestClient** `quackapi_request` | **shipped** — `quackapi_request(method, path [, body])` → status/body/content_type (no TCP) | `test/sql/quackapi_request.test` |
| **Rate limit (HTTP)** | **shipped** — `RATE LIMIT n PER s [BY ip\|token]` → 429 | `rate_limit.test.sh` |
| **ETag-304 / CACHE TTL** | not built (outbound `cache_httpfs` is separate) | SPEC cache_etag |
| **X-Request-ID + access log + `$request_id`** | **shipped** — uuidv7 mint, client header honor, SQL bind, stderr or operator-table access log | `quackapi_request_id.test`, `quackapi_access_log.test` |
| **curl_httpfs guarantee** | **shipped** — `http_client:='curl'` fails serve if missing; auto loud fallback | batteries + `curl_httpfs_client.test.sh` |
| **RFC 9457 problem+json** | FastAPI-shaped 422 only | `/tmp/quackapi_spec_problem_details/SPEC.md` |
| **Envelope** default JSON **array of rows**; `ENVELOPE object` + `EMPTY STATUS` shipped | array stays default; object/404 opt-in | `docs/reference/ddl.md`; `quackapi_envelope.test` |
| **Pydantic binder fidelity** native core shipped | field-level body `loc`, missing/null/default distinction, multi-error, typed BODY TYPE | `docs/PYDANTIC_PARITY.md` |
| **Multi-writer OLTP / wasm / unsupported Windows variants** | single-writer DuckDB; wasm, MinGW/rtools, and Windows arm64 are excluded in `description.yml` (Windows amd64 is supported) | packaging descriptor |

---

## 3. quack_from_X + PYDANTIC BRIDGE

### 3.1 Corpus numbers (commands on IR parquets)

| Lang | Routes IR | Model fields IR | Source report |
|------|----------:|----------------:|---------------|
| Python | **990** | **2908** | `/tmp/quackapi_corpus/PYTHON.md` |
| Ruby | **52** | **22** | `/tmp/quackapi_corpus/RUBY.md` |
| Go | **352** | **239** | `/tmp/quackapi_corpus/GO.md` |
| Node | **182** | **1035** | `/tmp/quackapi_corpus/NODE.md` |
| **Total** | **1576** | **4204** | parquet `count(*)` this session |
| Repos cloned | **36** top-level trees under `python/ ruby/ go/ node/` | | `ls -d /tmp/quackapi_corpus/*/*/` |

Python route tags (from PYTHON.md): fastapi 519 · drf_viewset 300 · flask 41 · …  
Clean Pydantic fields (exclude Django false tags): **1568** fields / 188 model×repo (`/tmp/quackapi_pydantic_bridge.md`).

OpenAPI-universal path (highest leverage): `/tmp/quackapi_corpus/SPEC.md` — `quack_from_openapi` / `quack_from_json_schema` design (petstore fixtures under `specs/`).

### 3.2 Framework metadata import

The two native table functions `quack_from_routes(framework, path)` and
`quack_from_models(framework, path)` support `fastapi`, `rails`, `express`,
and `gin`. They use sitting_duck ASTs and return route/model metadata, not
translated handlers or an automatically served application. The old eight
public names and importer SQL inspection helpers are removed.

Versioned framework fixtures, `quackapi_from_x*.test`, and
`quackapi_pydantic_contract.test` establish the current contract. FastAPI
required/default extraction handles bare Field(), metadata-only Field(),
ellipsis defaults and real defaults through AST structure.
See [the import guide](from-fastapi.md) and [bridge sources](../bridges/from_x/README.md).

Other frameworks and OpenAPI/JSON Schema import remain design/corpus work.
Historical corpus facade results below do not establish current automatic
translation coverage.

### 3.3 Pydantic → validation fidelity

From `/tmp/quackapi_pydantic_bridge.md` (live 8×200 + 12×422 curl transcript on locus ROH models):

| Band | Coverage | Mechanism |
|------|----------|-----------|
| **Real today (~81%)** | scalars, required/default, lists, nested, Literal/enum via schema, Field min/max/length/pattern via JSON Schema keywords, inheritance flatten | `BODY SCHEMA` + community **`json_schema`** + `PARAM` path/query constraints |
| **Native validation hardening** | BODY TYPE native DuckDB STRUCT/LIST/scalar bind with handler type derived from one declaration, aggregate parameter errors, explicit JSON-null vs missing/default, payload-aware JSON-Pointer body locations, and recursive OpenAPI requestBody emission | docs/PYDANTIC_PARITY.md, native binder + 422 emitter |
| **Still outside automatic Pydantic import** | Python custom validators/default factories and rich format checker policy (email/uri/datetime) | express as DuckDB SQL, JSON Schema, or composed extensions |
| Feature score (16-feature matrix) | **~78–81%** (full=1, partial=0.5) | §5 of pydantic_bridge |

**Compose-not-core:** EmailStr → `anofox_tabular` (or pattern); do not reimplement email in C++.

### 3.4 Handler A/B/C residue (FastAPI corpus)

From `/tmp/quackapi_handler_bridge.md` over **519** FastAPI-tagged routes:

| Bucket | Meaning | Count | % |
|--------|---------|------:|--:|
| **A** | Pure CRUD / declarative → `CREATE ROUTE AS SELECT/DML` | 498 | **96.0%** |
| **B** | Side-effect covered by existing ext (crypto/JWT/http_client/…) | 16 | **3.1%** |
| **C** | Imperative residue (all WebSocket demos in this corpus) | 5 | **1.0%** |
| **A+B auto-carry** | | 514 | **99.0%** |

**fastapi-realworld (n=19):** A 78.9% · B 21.1% · C 0% → **100% A+B**.  
**Realistic product prior:** ~**85–95%** of a CRUD FastAPI app (routes + validation + A + B) without new C++.

---

## 4. DESIGN STUDIES AND REMAINING GAPS

Twenty feasibility studies under `/tmp/quackapi_spec_*/SPEC.md`. The status
column is the current tree state; several studies describe surfaces that are
now shipped, while the remaining rows are partial or not built.

| # | Spec dir | One-line what | Effort | C++ vs compose | Shipped on main? |
|---|----------|---------------|:------:|----------------|------------------|
| 1 | `spec_api_versioning` | Path `/api/v1` vs `/v2`; optional DEPRECATED metadata | **S** | TRIVIAL-SQL; optional GROUP tags | **Partial** — GROUP/tags exist; deprecation sugar open |
| 2 | `spec_background_tasks` | AFTER-response / durable workers | **S** (docs) | **HAVE-EXT(`cronjob`)** + job table; SKIP `AFTER AS` | **Partial** — **`CREATE QUEUE`** is the durable path |
| 3 | `spec_body_partial` | PATCH partial / allowlist without model forks | **S** | **HAVE-EXT(`json_schema`)** + `json_merge_patch`; SKIP include/exclude C++ | **Partial** — BODY SCHEMA + SQL recipes |
| 4 | `spec_cache_etag` | `CACHE TTL` + ETag / If-None-Match → 304 | **M** | THIN-GLUE C++ + CORE table/hash | **No** |
| 5 | `spec_gzip` | `Accept-Encoding: gzip` / `zstd` response compression | **S** | THIN-GLUE (miniz/zstd already in DuckDB) | **Yes** — shipped in `quackapi_server.cpp` |
| 6 | `spec_health` | Liveness/readiness probes | **S** | **SKIP `CREATE PROBE`** — built-in routes | **Yes** — `/health` and `/healthz` |
| 7 | `spec_lifespan` | `on_start` / `on_stop` / drain on serve/stop | **S** | THIN-GLUE on serve/stop; HAVE-CORE scripts/ATTACH | **No** (script-before-serve works) |
| 8 | `spec_middleware` | `CREATE MIDDLEWARE … BEFORE\|AFTER` SQL hooks | **M** | THIN-GLUE registry in HandleRequest | **Yes** — ordinary routes and `quackapi_request` |
| 9 | `spec_oidc` | `CREATE AUTH … OIDC` login/callback SSO | **M–L** | PARTIAL-EXT(`quack_oauth`) + HttpFetch + sessions | **No** |
| 10 | `spec_pagination` | Offset + keyset list envelopes | **S** | **HAVE-CORE** LIMIT/OFFSET/WHERE | **Recipes** (no PAGINATE sugar) |
| 11 | `spec_problem_details` | RFC 9457 `application/problem+json` | **S** | THIN-GLUE format switch on emitters | **No** (FastAPI-shape only) |
| 12 | `spec_rate_limit` | RATE LIMIT + 429 + Retry-After | **S** | THIN-GLUE + in-process counters | **Yes** — `RATE LIMIT … BY ip\|token\|key` |
| 13 | `spec_request_id` | X-Request-ID + `$request_id` + access_log table | **S** | THIN-GLUE + core uuid/table | **Yes** — stderr and table-backed access logs |
| 14 | `spec_route_groups` | APIRouter-style prefix/tags/default auth | **S** | THIN-GLUE DDL expand | **Yes** — `CREATE GROUP` / `CREATE API GROUP` |
| 15 | `spec_serdes` | `FORMAT` JSON/NDJSON/CSV/Parquet/Arrow + Accept | **M** (S for JSON/NDJSON/CSV) | CORE writers + **HAVE-EXT(`nanoarrow`)** | **Yes** — explicit formats and default Accept negotiation |
| 16 | `spec_sessions` | Signed cookie sessions + CSRF | **M** | THIN-GLUE C++ cookie HMAC + table | **No** |
| 17 | `spec_static_files` | `static_prefix` + file/blob download + disposition | **S** | THIN-GLUE; httplib mount/ranges exist | **Partial** — `static_dir` only |
| 18 | `spec_streaming` | Chunked NDJSON/SSE + cancel on disconnect | **M** | THIN-GLUE ContentProvider + Interrupt | **Partial** — **`CREATE STREAM` SSE**; full NDJSON STREAM clause / cancel polish open |
| 19 | `spec_test_client` | `quackapi_request(…)` in-process TF | **S** | THIN-GLUE extract HandleRequest | **Yes** |
| 20 | `spec_websocket_sse` | Browser WS vs SSE+radio push | **M** (S if SSE-only docs) | SSE THIN-GLUE; **WS SKIP-BLOAT** on httplib; **HAVE-EXT(`radio`)** for bus | **Built** — SSE and WS streams |

### Ext adoption notes (composition, not new nouns)

| Report | Takeaway |
|--------|----------|
| `/tmp/quackapi_arrow.md` | Arrow/ADBC/Airport are **client** codecs/Flight clients — compose `nanoarrow` for FORMAT ARROW, do not embed Flight server in quackapi |
| `/tmp/quackapi_scope9.md` | **2/9 USEFUL-NOW** (netquack, inflector); skip false friends (http_stats, harbor-as-lib, nsv, query_condition_cache-as-HTTP-cache) |
| `/tmp/quackapi_scope_free.md` | **`ai`** + **`duckdb_mcp`** USEFUL-NOW for route AI tasks / MCP gateway without reinventing FastAPI-mcp |

---

## 5. INCREMENTAL LAUNCH ROADMAP

### v1 — **submit now** (= BUILT set)

The current built set declared by the repository's community descriptor includes:

- All **8 CREATE nouns** (ROUTE, AUTH, GROUP, API FOR TABLE, QUEUE, STREAM, ROW ACCESS POLICY, MASKING POLICY)
- Params / validation / body / form / multipart / headers / cookies / redirect / Set-Cookie  
- CORS + OPTIONS/HEAD/405 Allow  
- OpenAPI `/openapi.json` + `/docs` + `/redoc`  
- `quackapi_serve` / `stop` / `routes` / `servers` + static_dir + health routes + memory/resource guards
- Request IDs, table-backed access logs, response compression, rate limits, SQL middleware, formats, and `quackapi_request`
- Conformance **89/89 (100%)** + SQL/HTTP suites under `test/`  
- Platforms per descriptor: linux/osx amd64+arm64 and windows_amd64; exclude wasm, MinGW/rtools, and Windows arm64

**Gate:** green `test/conformance/run.sh` + `test/http/run_all.sh` + community packaging pin.

### v1.1 — ops + DX sugar (mostly **S**, thin C++)

| Feature | Effort | Why |
|---------|:------:|-----|
| Health/readiness **recipes** (no CREATE PROBE) | S | docs + HTTP composition tests |
| Pagination recipes (+ optional Link column) | S | HAVE-CORE |
| Static prefix + file/blob disposition | S | complete StaticFiles story |
| Lifespan `on_start`/`on_stop`/`drain_ms` | S | clean shutdown |
| Problem+json format switch | S | RFC 9457 option |
| Pydantic custom-validator/default-factory bridge | S–M | preserve explicit unsupported boundary without a Python runtime |

### v1.2 — identity, middleware, serdes ( **M** )

| Feature | Effort | Why |
|---------|:------:|-----|
| Sessions + CSRF | M | prerequisite for browser SSO |
| OIDC auth-code (`CREATE AUTH … OIDC`) | M (L if +sessions same train) | PARTIAL-EXT quack_oauth + HttpFetch |
| Cache TTL + ETag 304 | M | queryable cache table leapfrog |
| STREAM NDJSON + cancel-on-disconnect polish | M | completes streaming SPEC beyond SSE DDL |
| radio-composed multi-node SSE bus docs/tests | S–M | HAVE-EXT radio |

### Historical companion studies

Original bridge reports remain in `bridges/from_x/docs/` as historical evidence.
Temporary one-caller scripts and corpus extractors named by those reports are
not shipped entrypoints. Current extraction SQL is maintained in `src/sql/`,
embedded in the binary and exposed through the two metadata functions above.

### Blocked on transport

| Item | Blocker | Escape hatch |
|------|---------|--------------|
| Full Arrow Flight **server** inside quackapi | airport/adbc are **clients** | External Flight server; quackapi stays HTTP+JSON/IPC-blob |

---

## Evidence appendix (commands this session)

```text
# Built surface
rg 'StartsWith\(upper, "CREATE ' src/
/Users/aloksubbarao/personal/quackapi/build/release/duckdb -unsigned -c "LOAD quackapi; SELECT function_name FROM duckdb_functions() WHERE function_name ILIKE 'quackapi%' GROUP BY 1 ORDER BY 1;"

# Conformance re-run
PORT=18791 RESULTS_DIR=/tmp/quackapi_conformance_rerun bash test/conformance/run.sh
# → PASS 89 FAIL 0 N/A 0 · classes MATCH 88 STRONGER 1

# Corpus
duckdb -unsigned -c "… count(*) FROM read_parquet('/tmp/quackapi_corpus/ir_*_{routes,models}.parquet')"
# → routes 990+52+352+182=1576 · fields 2908+22+239+1035=4204
```

**Cited reports (not re-derived):**  
`/tmp/quackapi_fastapi_eq/SCORECARD.md` (stale 62/89),  
`/tmp/quackapi_{pydantic_bridge,fromfast,rails_bridge,handler_bridge}.md`,  
`/tmp/quackapi_corpus/{PYTHON,RUBY,GO,NODE,SPEC}.md`,  
`/tmp/quackapi_wanted/BACKLOG.md`, `/tmp/quackapi_next15/BACKLOG2.md`,  
`/tmp/quackapi_spec_*/SPEC.md` (×20),  
`/tmp/quackapi_{arrow,scope9,scope_free}.md`,  
in-repo `docs/FASTAPI_PARITY.md`, `description.yml`.

---

**Ledger line:** **The built set covers the 8 CREATE nouns plus the route/server/OpenAPI stack; 20 design studies remain, with the table above distinguishing open, partial, recipe, and shipped work.**
