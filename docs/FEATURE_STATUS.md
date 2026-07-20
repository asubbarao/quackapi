# quackapi — Feature Ledger (authoritative)

**Version:** 0.1.0 (community `description.yml`)  
**Tree:** `/Users/aloksubbarao/personal/quackapi` · branch `main` · HEAD `46fefed` (packaging pin `e254b43`)  
**Binary:** `build/release/duckdb -unsigned` (DuckDB v1.5.4, built 2026-07-19)  
**Rule:** every claim below is backed by a **command re-run this session** or a **cited report path**. No speculative status.

**Evidence index**

| Artifact | What it proves |
|----------|----------------|
| `rg 'CREATE …' src/` + `duckdb_functions()` | 8 CREATE nouns + 18 registered `quackapi_*` functions |
| `ls test/sql test/http test/conformance` | Versioned tests for each surface |
| `bash test/conformance/run.sh` → `/tmp/quackapi_conformance_rerun/` | **Live** FastAPI harness: **89/89 PASS (100%)** |
| `/tmp/quackapi_fastapi_eq/SCORECARD.md` | **Stale** baseline: **62/89 (69.7%)** pre body/form/multipart/cookies/headers/redirect/openapi/redoc |
| `/tmp/quackapi_corpus/{PYTHON,RUBY,GO,NODE,SPEC}.md` + IR parquets | 1576 routes · 4204 model fields · 36 repos |
| `/tmp/quackapi_{fromfast,rails_bridge,pydantic_bridge,handler_bridge}.md` | Bridge status + fidelity |
| `/tmp/quackapi_spec_*/SPEC.md` (20) | DESIGNED-not-built feasibility |
| `/tmp/quackapi_wanted/BACKLOG.md`, `/tmp/quackapi_next15/BACKLOG2.md` | Roadmap inputs |
| `/tmp/quackapi_{arrow,scope9,scope_free}.md` | Ext composition / free-win scope |

---

## 1. BUILT & MERGED

### 1.1 Eight CREATE nouns (parser extensions)

| # | CREATE noun | Implementation | Inspect / apply | Versioned tests | FastAPI equivalent |
|---|-------------|----------------|-----------------|-----------------|--------------------|
| 1 | **`CREATE [OR REPLACE] ROUTE`** | `src/quackapi_ddl.cpp` | `quackapi_routes()` | `test/sql/quackapi_routes.test`, `test/http/{routing,validation,body,body_schema,form,multipart,headers,cookies,redirect,trailing_slash}.test.sh` | `@app.get/post/…`, path/query/header/cookie/body params, status codes |
| 2 | **`CREATE [OR REPLACE] AUTH`** | `src/quackapi_auth.cpp` | `quackapi_auths()`, `quackapi_add_api_key()`, `quackapi_verify_auth()` | `test/sql/quackapi_auth.test`, `test/http/auth.test.sh` | `APIKeyHeader` / `HTTPBearer` + `Depends` |
| 3 | **`CREATE [OR REPLACE] [API] GROUP`** | `src/quackapi_ddl.cpp` | `quackapi_groups()` | `test/sql/quackapi_group.test`, `test/http/group.test.sh` | `APIRouter(prefix=…, tags=…, dependencies=…)` |
| 4 | **`CREATE [OR REPLACE] API FOR TABLE`** | `src/quackapi_table_api.cpp` | expands to routes → `quackapi_routes()` | `test/sql/quackapi_table_api.test` | FastAPI + SQLAlchemy list/get scaffold |
| 5 | **`CREATE [OR REPLACE] QUEUE`** | `src/quackapi_queue.cpp` | `quackapi_queues()`, `quackapi_enqueue/dequeue/ack/nack`, table `quackapi_jobs` | `test/sql/quackapi_queue.test`, `test/http/queue.test.sh` | BackgroundTasks / Celery-shaped durable jobs (DB-native) |
| 6 | **`CREATE [OR REPLACE] STREAM`** | `src/quackapi_stream.cpp` | `quackapi_streams()` | `test/sql/quackapi_stream.test`, `test/http/stream.test.sh` | SSE (`EventSourceResponse`); **WS rejected** (httplib) |
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
| **Form** `application/x-www-form-urlencoded` | body binder | `form.test.sh`, case `form_submit` | `Form()` |
| **Multipart** fields + file (`$file`, `$filename`) | body binder | `multipart.test.sh`, case `multipart_upload` | `File()`, `UploadFile` |
| **STATUS n** | `STATUS 201` etc. | conformance `status_*` | `status_code=` / `Response` |
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
| Serve / stop / inspect | `quackapi_serve([port], host, static_dir, cors_origins, memory_limit)`, `quackapi_stop`, `quackapi_servers` | `fiveliner.test.sh`, `memory_limit` SQL test | `uvicorn` / `app` process |
| **CORS** | `cors_origins` arg / `SET quackapi_cors_origins` | `test/sql/quackapi_cors.test`, `test/http/cors.test.sh` | `CORSMiddleware` |
| **static_dir** unrouted GETs | `quackapi_serve(…, static_dir := …)` | description.yml / README | `StaticFiles` (mount; prefix sugar still SPEC) |
| **OpenAPI 3.1** | built-in `GET /openapi.json` (`quackapi_openapi.cpp`) | `test/sql/quackapi_openapi.test`, case `openapi_json` | auto OpenAPI |
| **Swagger UI** | `GET /docs` | same + case `docs_get` | `/docs` |
| **ReDoc** | `GET /redoc` | `test/http/redoc.test.sh`, case `redoc_get` | `/redoc` |
| **Queue** enqueue/dequeue/ack/nack | `quackapi_enqueue` / `_dequeue` / `_ack` / `_nack`, `quackapi_jobs` | queue SQL + HTTP tests | BackgroundTasks + broker |
| **SSE stream** | `CREATE STREAM … GET '/path' AS <select>` | stream SQL + HTTP | `StreamingResponse` / SSE |
| **WebSocket** | **not built** — DDL errors with explicit httplib message | stream tests document reject | `WebSocket` |
| **Row access + masking policies** | CREATE … POLICY + ALTER TABLE bind | policy SQL + HTTP | — (DB-native leapfrog) |
| Quack RPC auth bridge | `quackapi_authentication` / `quackapi_authorization` | `quackapi_quack_bridge.test` | N/A (DuckDB quack sibling) |
| Security / key hashing | auth + security tests | `quackapi_security.test` | secret storage patterns |

### 1.4 Registered SQL functions (live `duckdb_functions()`)

```
quackapi_ack, quackapi_add_api_key, quackapi_authentication, quackapi_authorization,
quackapi_auths, quackapi_dequeue, quackapi_enqueue, quackapi_groups, quackapi_http_util_name,
quackapi_nack, quackapi_policies, quackapi_queues, quackapi_routes, quackapi_serve,
quackapi_servers, quackapi_stop, quackapi_streams, quackapi_verify_auth
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
| **WebSocket Upgrade** | Bundled cpp-httplib has no WS; `CREATE STREAM … WS` rejected | `src/quackapi_stream.cpp` error string; `/tmp/quackapi_spec_websocket_sse/SPEC.md` SKIP-BLOAT |
| **OIDC / OAuth2 SSO** browser code flow | JWT/API_KEY only today | `/tmp/quackapi_spec_oidc/SPEC.md` |
| **Signed cookie sessions + CSRF** | not built | `/tmp/quackapi_spec_sessions/SPEC.md` |
| **Middleware BEFORE/AFTER SQL** | not built | `/tmp/quackapi_spec_middleware/SPEC.md` |
| **Response gzip** | not wired (miniz available) | `/tmp/quackapi_spec_gzip/SPEC.md` |
| **FORMAT / Accept** (CSV/NDJSON/Arrow IPC) | JSON/html/text only | `/tmp/quackapi_spec_serdes/SPEC.md`, `/tmp/quackapi_arrow.md` |
| **In-process TestClient** `quackapi_request` | network tests only | `/tmp/quackapi_spec_test_client/SPEC.md` |
| **Rate limit / ETag-304 / request-id access log** | designed, not built | SPECs rate_limit, cache_etag, request_id |
| **RFC 9457 problem+json** | FastAPI-shaped 422 only | `/tmp/quackapi_spec_problem_details/SPEC.md` |
| **Envelope** always JSON **array of rows** | intentional SQL semantics (harness still MATCH on fields) | `docs/FASTAPI_PARITY.md` |
| **Pydantic binder fidelity** ~19% needs C++ | field-level body `loc`, optional/null body, multi-error | `/tmp/quackapi_pydantic_bridge.md` |
| **Multi-writer OLTP / wasm / Windows** | single-writer DuckDB; platforms excluded in `description.yml` | packaging descriptor |

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

### 3.2 Per-framework bridge status

| Bridge | Status | Proof | Next |
|--------|--------|-------|------|
| **`quack_from_fastapi`** | **partial one-caller** (shell + pure SQL, not in-tree C++ TF yet) | fastapi-realworld **pass=10 fail=0** (19 routes registered, 25 models); locus ROH **pass=10 fail=0** | C++ `quack_from_fastapi(path)` TVF | `/tmp/quackapi_fromfast.md` |
| **`quack_from_rails`** | **proven one-caller (partial runtime fidelity)** | rails-realworld **pass=14 fail=0**; handlers = SQL façades not Ruby | `quack_from_express` | `/tmp/quackapi_rails_bridge.md` |
| **`quack_from_openapi` / JSON Schema** | **designed + corpus specs**; highest-leverage multi-stack path | SPEC + petstore YAML fixtures | bulk emit + serve recipe | `/tmp/quackapi_corpus/SPEC.md` |
| **Express / Nest / Koa / Fastify** | **IR only** (Node corpus 182 routes) | NODE.md | shell bridge not shipped as one-caller |
| **Gin / Echo / Fiber / chi** | **IR only** (Go corpus 352 routes) | GO.md | `quack_from_gin` listed next after rails |
| **DRF / Flask / Django** | **IR extracted** (Python 990 routes) | PYTHON.md | no dedicated one-caller beyond FastAPI path |
| **Sinatra** | **IR + synthetic RealWorld** | RUBY.md | secondary to Rails |

### 3.3 Pydantic → validation fidelity

From `/tmp/quackapi_pydantic_bridge.md` (live 8×200 + 12×422 curl transcript on locus ROH models):

| Band | Coverage | Mechanism |
|------|----------|-----------|
| **Real today (~81%)** | scalars, required/default, lists, nested, Literal/enum via schema, Field min/max/length/pattern via JSON Schema keywords, inheritance flatten | `BODY SCHEMA` + community **`json_schema`** + `PARAM` path/query constraints |
| **Needs C++ (~19%)** | field-level body `loc` parity, optional/null body bind defaults, multi-error aggregation, strict `format:` checkers (email/uri/uuid/datetime) | binder + 422 emitter work |
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

## 4. DESIGNED, NOT BUILT

Twenty feasibility studies under `/tmp/quackapi_spec_*/SPEC.md`.  
**Note:** some peers **shipped after** the SPEC was written (GROUP, STREAM SSE, BODY SCHEMA, OpenAPI). Rows still list the SPEC intent; **shipped** column is live tree truth (this session).

| # | Spec dir | One-line what | Effort | C++ vs compose | Shipped on main? |
|---|----------|---------------|:------:|----------------|------------------|
| 1 | `spec_api_versioning` | Path `/api/v1` vs `/v2`; optional DEPRECATED metadata | **S** | TRIVIAL-SQL; optional GROUP tags | **Partial** — GROUP/tags exist; deprecation sugar open |
| 2 | `spec_background_tasks` | AFTER-response / durable workers | **S** (docs) | **HAVE-EXT(`cronjob`)** + job table; SKIP `AFTER AS` | **Partial** — **`CREATE QUEUE`** is the durable path |
| 3 | `spec_body_partial` | PATCH partial / allowlist without model forks | **S** | **HAVE-EXT(`json_schema`)** + `json_merge_patch`; SKIP include/exclude C++ | **Partial** — BODY SCHEMA + SQL recipes |
| 4 | `spec_cache_etag` | `CACHE TTL` + ETag / If-None-Match → 304 | **M** | THIN-GLUE C++ + CORE table/hash | **No** |
| 5 | `spec_gzip` | `Accept-Encoding: gzip` response compress | **S** | THIN-GLUE (miniz already in DuckDB) | **No** |
| 6 | `spec_health` | Liveness/readiness probes | **S** | **SKIP `CREATE PROBE`** — TRIVIAL-SQL routes | **Recipes only** (no DDL) |
| 7 | `spec_lifespan` | `on_start` / `on_stop` / drain on serve/stop | **S** | THIN-GLUE on serve/stop; HAVE-CORE scripts/ATTACH | **No** (script-before-serve works) |
| 8 | `spec_middleware` | `CREATE MIDDLEWARE … BEFORE\|AFTER` SQL hooks | **M** | THIN-GLUE registry in HandleRequest | **No** |
| 9 | `spec_oidc` | `CREATE AUTH … OIDC` login/callback SSO | **M–L** | PARTIAL-EXT(`quack_oauth`) + HttpFetch + sessions | **No** |
| 10 | `spec_pagination` | Offset + keyset list envelopes | **S** | **HAVE-CORE** LIMIT/OFFSET/WHERE | **Recipes** (no PAGINATE sugar) |
| 11 | `spec_problem_details` | RFC 9457 `application/problem+json` | **S** | THIN-GLUE format switch on emitters | **No** (FastAPI-shape only) |
| 12 | `spec_rate_limit` | RATE LIMIT + 429 + Retry-After | **S** | THIN-GLUE + CORE counter table | **No** |
| 13 | `spec_request_id` | X-Request-ID + `$request_id` + access_log table | **S** | THIN-GLUE + CORE uuid/table | **No** |
| 14 | `spec_route_groups` | APIRouter-style prefix/tags/default auth | **S** | THIN-GLUE DDL expand | **Yes** — `CREATE GROUP` / `CREATE API GROUP` |
| 15 | `spec_serdes` | `FORMAT` JSON/NDJSON/CSV/Parquet/Arrow + Accept | **M** (S for JSON/NDJSON/CSV) | CORE writers + **HAVE-EXT(`nanoarrow`)** | **No** (json/html/text only) |
| 16 | `spec_sessions` | Signed cookie sessions + CSRF | **M** | THIN-GLUE C++ cookie HMAC + table | **No** |
| 17 | `spec_static_files` | `static_prefix` + file/blob download + disposition | **S** | THIN-GLUE; httplib mount/ranges exist | **Partial** — `static_dir` only |
| 18 | `spec_streaming` | Chunked NDJSON/SSE + cancel on disconnect | **M** | THIN-GLUE ContentProvider + Interrupt | **Partial** — **`CREATE STREAM` SSE**; full NDJSON STREAM clause / cancel polish open |
| 19 | `spec_test_client` | `quackapi_request(…)` in-process TF | **S** | THIN-GLUE extract HandleRequest | **No** |
| 20 | `spec_websocket_sse` | Browser WS vs SSE+radio push | **M** (S if SSE-only docs) | SSE THIN-GLUE; **WS SKIP-BLOAT** on httplib; **HAVE-EXT(`radio`)** for bus | **Partial** — SSE built; **WS blocked on transport** |

### Ext adoption notes (composition, not new nouns)

| Report | Takeaway |
|--------|----------|
| `/tmp/quackapi_arrow.md` | Arrow/ADBC/Airport are **client** codecs/Flight clients — compose `nanoarrow` for FORMAT ARROW, do not embed Flight server in quackapi |
| `/tmp/quackapi_scope9.md` | **2/9 USEFUL-NOW** (netquack, inflector); skip false friends (http_stats, harbor-as-lib, nsv, query_condition_cache-as-HTTP-cache) |
| `/tmp/quackapi_scope_free.md` | **`ai`** + **`duckdb_mcp`** USEFUL-NOW for route AI tasks / MCP gateway without reinventing FastAPI-mcp |

---

## 5. INCREMENTAL LAUNCH ROADMAP

### v1 — **submit now** (= BUILT set)

Ship community-extensions `description.yml` **0.1.0** with the surface proven on main:

- All **8 CREATE nouns** (ROUTE, AUTH, GROUP, API FOR TABLE, QUEUE, STREAM, ROW ACCESS POLICY, MASKING POLICY)
- Params / validation / body / form / multipart / headers / cookies / redirect / Set-Cookie  
- CORS + OPTIONS/HEAD/405 Allow  
- OpenAPI `/openapi.json` + `/docs` + `/redoc`  
- `quackapi_serve` / `stop` / `routes` / `servers` + static_dir + memory_limit guard  
- Conformance **89/89 (100%)** + SQL/HTTP suites under `test/`  
- Platforms per descriptor: linux/osx amd64+arm64; **exclude** wasm + Windows until green CI  

**Gate:** green `test/conformance/run.sh` + `test/http/run_all.sh` + community packaging pin.

### v1.1 — ops + DX sugar (mostly **S**, thin C++)

| Feature | Effort | Why |
|---------|:------:|-----|
| Request ID + access_log table | S | Ops baseline |
| Rate limit (table counters → 429) | S | Production table stakes |
| GZip responses | S | miniz already in tree |
| Health/readiness **recipes** (no CREATE PROBE) | S | docs + HTTP composition tests |
| Pagination recipes (+ optional Link column) | S | HAVE-CORE |
| Static prefix + file/blob disposition | S | complete StaticFiles story |
| `quackapi_request` TestClient TF | S | CI without ports |
| Lifespan `on_start`/`on_stop`/`drain_ms` | S | clean shutdown |
| Problem+json format switch | S | RFC 9457 option |
| Body optional/null binder + multi-error 422 polish | S–M | close Pydantic ~19% gap |

### v1.2 — identity, middleware, serdes ( **M** )

| Feature | Effort | Why |
|---------|:------:|-----|
| Middleware BEFORE/AFTER SQL | M | hosts rate/cache/audit declaratively |
| Sessions + CSRF | M | prerequisite for browser SSO |
| OIDC auth-code (`CREATE AUTH … OIDC`) | M (L if +sessions same train) | PARTIAL-EXT quack_oauth + HttpFetch |
| Cache TTL + ETag 304 | M | queryable cache table leapfrog |
| FORMAT NDJSON/CSV then Parquet/Arrow | M | compose nanoarrow; Accept negotiation |
| STREAM NDJSON + cancel-on-disconnect polish | M | completes streaming SPEC beyond SSE DDL |
| radio-composed multi-node SSE bus docs/tests | S–M | HAVE-EXT radio |

### Ships as companion (bridges — not blocking community submit)

| Companion | Form | Status |
|-----------|------|--------|
| `quack_from_fastapi.sh` | shell + SQL, **homed in-repo**: `bridges/from_x/one_callers/` | partial one-caller, e2e green (corpus + fixture in `bridges/from_x/fixtures/fastapi_mini`, `test/sql/quackapi_from_x_bridge.test`) |
| `quack_from_rails` | same shape, corpus proof homed in `bridges/from_x/docs/rails_bridge.md` | proven rails-realworld |
| `quack_from_openapi` | pure SQL over OpenAPI/JSON Schema | designed + fixtures |
| Pydantic → BODY SCHEMA emitter | SQL/scripts, extraction homed in `bridges/from_x/extract/extract_python_ir.sql` | ~81% feature map |
| Future: express / gin / DRF one-callers | IR extractors homed in `bridges/from_x/extract/` (node/go); ruby extractor `bridges/from_x/extract/extract_ruby_ir.py` | not shipped as one-callers |

**Promoted into-repo:** `bridges/from_x/` (branch `feat/from-x`) now carries the extraction SQL (Python/Node/Go/Ruby), the `quack_from_fastapi` one-caller driver, a committed fixture, and a passing sqllogictest — closing the "promote after v1" TODO below for the extraction half. The one-caller is still shell+SQL, not an in-tree C++ `quack_from_*(path)` TVF; that remains open (see `bridges/from_x/README.md` §4 and the P0 items in `bridges/from_x/docs/fromfast.md` §4).

### Blocked on transport

| Item | Blocker | Escape hatch |
|------|---------|--------------|
| **WebSocket Upgrade** (browser RFC6455 server) | cpp-httplib **no** WebSocket/Upgrade API | **SSE** (`CREATE STREAM`) + **`radio`** for push bus; duplex RPC → **quack** protocol, not DIY WS |
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

**Ledger line:** **built ≈ 30+ first-class surfaces (8 CREATE nouns + full route/server/OpenAPI stack); designed-not-built ≈ 20 SPECs (≈12 still fully open, ≈8 partial/shipped-or-recipe); FastAPI harness parity old 62/89 (69.7%) → refreshed 89/89 (100%).**
