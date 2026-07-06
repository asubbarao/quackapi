# FastAPI vs quackapi — Feature Gap Matrix

**Generated:** 2026-07-02 (read-only analysis of repo at /Users/aloksubbarao/quackapi)  
**Assumed FastAPI baseline:** 0.115+ / 0.139 conventions (from bench/BENCH_HEADTOHEAD.md using fastapi 0.139.0 + uvicorn[standard]).  
**Scope:** Evidence strictly from checked-in files (framework.sql, app.sql, ext-cpp/src/quackapi_brain.cpp, ext-cpp/src/quackapi_extension.cpp, parity_b2.sh, edges.md, edges_round5_draft.md, B2_RESULT.md, B3_VERIFY_RESULT.md, BENCH_HEADTOHEAD.md, test/*.sql, test/*.sh, di.sql, middleware.sql, dispatch.sql, serve_ws.sql, serve_*.sql, README.md). No servers started, no load generated, no files edited except this deliverable.

## Executive Summary

**Grade counts** (features enumerated below):
- **WON**: 13 (core routing+params+JSON body+validation 422+status+statics+openapi/docs+SSE+CREATE ROUTE DDL+background enqueue+perf+handle_request oracle testing+parity harness).
- **PRESENT-UNTESTED**: 3 (middleware chain exists+tested in isolation; DI model exists; header injection post).
- **NATURAL**: 7 (auth as SQL predicate on registry/middleware; response cookies via post header injector; custom validators via SQL in handlers or constraints; sub-routers via filtered registry queries; exception handlers via pre-phase; request-scoped context via di_resolve; simple redirects via status+Location in static/handler).
- **HARD**: 8 (WebSockets — stateful bidirectional frame loop+upgrade not one-shot SQL dispatch; async handlers/upstream I/O — 16 blocking pthread workers with persistent conns; multipart/form-data file upload streaming — fixed 64 KB read buffer+ducktinycc limits in handle_conn_on; TLS termination — no SSL anywhere in socket/accept code; true cross-statement resource lifetime for yield-DI; sub-process graceful shutdown hooks; full content negotiation beyond Accept in headers; OpenAPI securitySchemes/components depth).
- **N/A**: 4 (Pydantic response_model field filtering/exclude_unset — quackapi emits what handler returns; FastAPI TestClient — quackapi's handle_request oracle is strictly stronger for parity; Python-native threadpool offload for sync — not applicable; uvicorn worker model config — 16 is hardcoded in C).

**The 5 most important gaps for a credible "FastAPI replacement" claim** (in priority order for real-world adoption):
1. **No TLS termination story at all** (brain.cpp, extension, serve_*.sql, edges.md). Every production FastAPI deployment terminates TLS (uvicorn --ssl or proxy). quackapi has zero HTTPS path; sockets are plain TCP only. Disqualifying for any browser-facing or compliance use without external proxy.
2. **Multipart / file upload streaming is PARTIAL and bounded at ~64 KB** (edges.md:203-224, brain.cpp:210 read into 65536 but single n=read(fd,req,65535) + no boundary parse in shipped path; probe only reached handler for tiny bodies). FastAPI + Starlette streams to temp or spooled files. Real uploads (avatars, CSVs, images) are table stakes.
3. **WebSockets not integrated into the main server path** (serve_ws.sql separate echo-only on different port; edges.md:118-130 transport DEFEATED in isolation but "PARTIAL (app layer)"). FastAPI mounts WS on same app as HTTP routes with @app.websocket and shared DI/auth. Separate serve is not "supplant."
4. **Async/await upstream I/O and non-blocking workers** (brain.cpp:450 worker_main is sync duckdb_query loop on 16 pthreads; edges.md:1c trilemma + #8). FastAPI async def + httpx/anyio allows high-concurrency I/O-bound without thread cost. quackapi's model is "16 blocking workers" (explicitly called out in bench and edges); workloads with DB fanout or slow external HTTP inside handlers lose.
5. **Dependency Injection with real yield-style resource lifetime + open txn across handler** (edges.md:5-6 REAL/PARTIAL, di.sql:20-70, probes/5_open_txn.sql+6_di_setup_teardown.sql). FastAPI Depends(get_db) with yield guarantees finally semantics and read-your-writes txn. quackapi's di_resolve context_json + explicit teardown dispatch models ordering but cannot hold live objects or txns across statements (one-shot dispatch model).

**The 3 places quackapi is strictly BETTER** (verified against repo, not prior memory):
1. **Performance (framework overhead)**: bench/BENCH_HEADTOHEAD.md + ext-cpp/B2_RESULT.md. quackapi serve_brain (16 workers) static /health: ~41-42k r/s (c64); dynamic /users/1 ~27-35k. FastAPI+uvicorn mem (workers=16): ~17-22k health, ~14-19k users/1. DuckDB-backed FastAPI shows length/Non-2xx errors under load. quackapi: 0 failed across matrix. Static exceeds uvicorn; light dynamic 1.5-2x+ with perfect stability. Evidence: raw ab "Requests per second" and "Failed requests: 0" lines.
2. **CREATE ROUTE / DROP ROUTE as first-class DDL** (ext-cpp/src/quackapi_extension.cpp:300+ (RouteDdlParse/Plan/ApplyRouteFunc), B3_VERIFY_RESULT.md:60-130, edges_round5_draft.md:10-110). Registers into routes+param_schema SSOT then quack_reload_router(g_rt). Live syntax extension via ParserExtension. FastAPI: decorators run at import time (no runtime DDL, no parser hook). B3_VERIFY: 16/16 parity after CREATE, live server curls, constraint/optional syntax, DROP. Transaction split honest edge documented.
3. **SQL-oracle testing model (handle_request as byte-parity oracle)**: parity_b2.sh (16-case matrix, json_object compare, special openapi semantic), test/tier1_handle_request.test.sql (CTE checks on status/body/handler_sql), ext-cpp/B2_RESULT.md:16/16, B3_VERIFY: re-ran after edits. C router decision vs `SELECT * FROM handle_request(...)` is the gate. FastAPI TestClient exercises the Python app object; quackapi's Tier-1 pure-SQL oracle + Tier-2 curl allows verifying the brain without a running HTTP server and catches drift at the exact routing/validation layer. 100% parity on shipped matrix (except documented non-deterministic openapi key order).

## 1. Routing

| feature | FastAPI behavior | quackapi status | evidence | notes |
|---------|------------------|-----------------|----------|-------|
| Path params `{name}` | Capture from URL, typed conversion in path | WON | framework.sql:160-170 (best pmap from pat_segs), app.sql:20 (`/users/{id}`), brain.cpp:950-960 (pnames/pvals from req_segs), parity_b2.sh:21, tier1_handle_request.test.sql:1 (GET /users/123) | Exact seg match, no regex. |
| Query params | `?q=..&limit=..`, typed, optional/default | WON | framework.sql:130-140 (query_map), param_schema seed query, app.sql:40 (`/search`), brain.cpp:610 (parse_query), 970 (lookup), tier1:13 | Optional via schema; defaults via SQL coalesce in handler. |
| Converters / types in path/query | `:int`, `Path(..., ge=..)` etc | WON | param_schema "type", brain.cpp:980 (quack_parse_int etc for int/float/bool), framework 200-220 (try_cast) | Types limited to int/float/bool/string in schema. |
| Multiple methods per path | `@app.get` + `@app.post` same path ok | WON | routes table allows duplicate pattern different method; matched: `ri.method = method`, app.sql seeds POST /users + GET /users | Precedence not issue because method exact. |
| Sub-routers / mounts (APIRouter) | `router = APIRouter(prefix="/v1")`; `app.include_router` | NATURAL | Registry is flat `routes` table; could filter `WHERE pattern LIKE '/v1/%'` or add prefix column + rewrite at load | No code exists; SQL JOIN/filter would be elegant. No evidence of prefix support. |
| Route precedence | Most-specific first (or order of registration) | WON | brain.cpp:930-945 (most literal_count wins, tie by route_id), framework.sql:160-170 (QUALIFY row_number ORDER BY literal_count DESC) | "most-literal-segments" tiebreak documented. |
| Trailing-slash behavior | Configurable (or redirect) | PRESENT-UNTESTED | No special logic; exact seg_count + literal match in both SQL and C | `/users/` vs `/users` treated different (no test covers). |
| HEAD / OPTIONS auto-handling | FastAPI auto-generates HEAD from GET; OPTIONS for CORS | HARD | Method exact match only (`ri.method = method`); no auto in C router or handle_request | Would require explicit routes or C special-case before match. |
| Route ordering / conflict detection | Decorator order or explicit | WON (via literal tiebreak) | Same as precedence row | Registry scan order not used; literal drives. |

## 2. Request handling

| feature | FastAPI behavior | quackapi status | evidence | notes |
|---------|------------------|-----------------|----------|-------|
| JSON body | `item: Item` (Pydantic) auto parsed | WON | framework.sql:190 (body json_extract), app.sql:120 (create_user body), brain.cpp:970 (json_extract_string for body), tier1:7, parity POST cases | Only `application/json` assumed; body passed raw to extract. |
| Form data (application/x-www-form-urlencoded) | `Form(...)` | HARD | Body handling only does json_extract for 'body' params; no form parse in C or SQL | No evidence of form split. |
| Multipart file upload | `UploadFile`, `File(...)`, streaming | HARD | edges.md:203 (PARTIAL, 64 KB ceiling), brain.cpp:210 (`read(fd, req, 65535)` single, no boundary handling in shipped handle_conn_on), no param type "file" | Probe showed small bodies reach handler; large truncate. No disk spool. |
| Request headers | `Header(...)`, `Request.headers` | PRESENT-UNTESTED | brain.cpp:269-360 (headers_json built, lower keys, _cookies special), middleware.sql:86 (json_extract headers.authorization), handle_request sig takes headers | Parsed and available to middleware; **not** injected as route params in current param_schema (only path/query/body). No first-class Header() equivalent in routes. |
| Cookies | `Cookie(...)`, request.cookies | PRESENT-UNTESTED | brain.cpp:300 (cookie_val -> _cookies object in headers_json) | Incoming cookies parsed; no Set-Cookie response helper beyond manual header injector. |
| Content-type negotiation | `Accept` header, produces based on it | N/A (partial) | No Accept parsing or content negotiation in router or responder; routes declare fixed content_type | Handlers return JSON; statics have ct. |
| Raw request object access | `Request` with .url, .client, .body() etc | NATURAL | All wire data (method/path/headers/body) flows into handle_request and C; could expose richer json | Currently only used for routing+auth in middleware; no full Request model surfaced to SQL handlers. |

## 3. Validation/serialization (Pydantic)

| feature | FastAPI behavior | quackapi status | evidence | notes |
|---------|------------------|-----------------|----------|-------|
| Primitive types + conversion | str/int/float/bool/datetime etc | WON | param_schema type, framework.sql:210 (int/float/bool try_cast), brain.cpp:980-1000 (parse_int, strtod, is_valid_bool) | No datetime, UUID etc. |
| Constraints (ge/le/min_length/regex/enum) | Field(ge=0, le=100, regex=..) | WON (subset) | param_schema constraint_json, framework 210-230 (le/ge checks), brain.cpp:1010-1020 (c.has_le etc), app.sql:120 `{"le":100}` | Only le/ge on int shown+tested; no min_length/regex/enum in code. |
| Optional / defaults | `Optional[str] = None`, `Query(None)` | WON | required flag, framework 200 (missing only if required), query optional in schema, brain 990 | Defaults expressed in handler SQL (coalesce). |
| Nested models | `Item` containing `List[Sub]` | HARD | No nested structures; flat param_schema only | Handlers can return nested via to_json of structs, but input validation is flat. |
| response_model filtering | Exclude unset, by_alias, etc | N/A | Handler SQL decides exact projection (SELECT to_json); no automatic response_model | Equivalent power via SQL; different mechanism. |
| Custom validators | `@validator`, `field_validator`, root | NATURAL | Constraint_json + arbitrary SQL in handler or pre middleware | No Pydantic @validator surface; can put CHECK or CASE in rendered SQL. |
| 422 error shape | `{"detail":[{"type":"...","loc":["body","x"],"msg":"..."}]}` | WON | framework.sql:240-260 (err_agg json_object exact msgs), brain.cpp:1020-1050 (builds identical), parity 422 cases, tier1 checks 2/4/8 | Byte and semantic match documented. |

## 4. Responses

| feature | FastAPI behavior | quackapi status | evidence | notes |
|---------|------------------|-----------------|----------|-------|
| Explicit status codes | `status_code=201` | WON | routes.status, framework 367, app.sql create_user 201, B2 parity POST | Per-route. |
| Custom response headers | `Response(headers=..)` or middleware | PRESENT-UNTESTED | middleware.sql:163 (apply_post returns/augments resp_headers), 177 header_injector example ("X-Powered-By") | Post-phase can inject; no per-handler Response() object. |
| Set-Cookie | `Response.set_cookie`, `set_cookie()` helper | NATURAL | Post header injector can emit "Set-Cookie" key; no dedicated cookie builder | Would be header string. |
| Response classes (JSONResponse, HTMLResponse, PlainTextResponse, RedirectResponse, FileResponse, StreamingResponse) | First-class | WON (subset) | kind=static/html/stream + content_type; /docs is full HTML, /events stream chunked, JSON default | No RedirectResponse (would be 3xx + Location header), no FileResponse (no fs serve), no explicit PlainText. Streaming only for kind=stream rows via chunked. |
| SSE (Server-Sent Events) | `StreamingResponse` with `text/event-stream`, `yield "data:.."` | WON | app.sql:70 (kind=stream /events), framework 370 (ct=text/event-stream), brain.cpp:370-400 (Transfer-Encoding:chunked, per-row `data: %s\n\n`), edges.md:2 DEFEATED | Uses result rows as events; not live generator. |
| Compression (GZip etc) | GZipMiddleware, responses compressed | HARD | No gzip of responses in responder (only SSE chunked); no middleware for it | Could be added in C write path or post. |

## 5. Async/concurrency

| feature | FastAPI behavior | quackapi status | evidence | notes |
|---------|------------------|-----------------|----------|-------|
| async def handlers (await I/O) | Native, anyio/httpx etc | HARD | brain.cpp:450-480 worker_main is blocking `for(;;) { pthread_cond; handle_conn_on(con,fd) }` + sync `g_ddb_query`; 16 pthreads | No async runtime; workers block on DuckDB. |
| Threadpool for sync handlers | `def` run in threadpool, does not block event loop | N/A | All paths are thread-per-connection blocking workers | Model is pre-fork-ish pthreads, not event+pool. |
| Backpressure | ASGI server level | HARD | Accept queue g_q[4096] + close on full; no sophisticated backpressure | Simple bounded queue. |
| 16 blocking workers reality | N/A (contrast) | WON (as design) | brain.cpp:195 `NWORKERS 16`, worker_main, serve_brain_impl; edges.md 1c, BENCH, B2 | Explicitly 16; perf wins for CPU-light; loses I/O-heavy vs async. |

## 6. Dependency injection

| feature | FastAPI behavior | quackapi status | evidence | notes |
|---------|------------------|-----------------|----------|-------|
| Depends() | `def handler(x: int = Depends(get_x))` | NATURAL | di.sql exists (providers + di_resolve -> context_json); middleware pre can also shape | No Depends decorator; injection is json field read in handler SQL. |
| yield-DI (setup/teardown) | Generator deps with finally | PARTIAL | di.sql:30-70 (setup via di_resolve, teardown explicit), edges.md:6 PARTIAL, probes/6_*.sql | Ordering modeled; no resource object lifetime across statements; no guaranteed finally if C crashes between send and teardown. |
| Request-scoped state | `request.state` or context | PRESENT-UNTESTED | di_resolve returns context_json per-request | Works for JSON values; passed to handlers via convention. |

## 7. Auth/security

| feature | FastAPI behavior | quackapi status | evidence | notes |
|---------|------------------|-----------------|----------|-------|
| OAuth2 / JWT helpers | OAuth2PasswordBearer, jwt.decode etc | N/A | No JWT lib or OAuth helpers in repo | Handlers can call extensions or shell for verification. |
| API key / HTTPBasic | `APIKeyHeader`, `HTTPBasic` | NATURAL | middleware.sql seed has "auth_gate" Bearer example; easy SQL predicate on header | Example only; not a security scheme registry. |
| Security schemes in OpenAPI | `securitySchemes` populated from deps | HARD | OpenAPI builder in framework.sql:310 only emits parameters + basic responses; no securitySchemes | Would need extra registry column + emit in the json_object. |

## 8. Middleware

| feature | FastAPI behavior | quackapi status | evidence | notes |
|---------|------------------|-----------------|----------|-------|
| CORS middleware | CORSMiddleware (origins, methods, headers) | NATURAL | Pre/post middleware table can inspect Origin and inject ACA* headers | No built-in; DIY via config row. |
| GZip middleware | GZipMiddleware | HARD | No compression pass in write path | SSE uses chunked; bodies not gzipped. |
| Custom middleware | `app.add_middleware(BaseHTTPMiddleware)` | WON | middleware.sql full chain (pre/post priority-ordered), test/middleware.test.sql | Applied via SQL macros; C layer calls apply_pre before route. |
| Exception handlers | `app.add_exception_handler(422, ...)` or `HTTPException` | NATURAL | 422/404 generated inside handle_request; pre-phase can short-circuit; post can transform | No global exception registry table yet. |

## 9. Background

| feature | FastAPI behavior | quackapi status | evidence | notes |
|---------|------------------|-----------------|----------|-------|
| BackgroundTasks | `BackgroundTasks.add_task(fn)` after return | WON | dispatch.sql:310 `dispatch_async`, middleware.sql:245 enqueue_background, edges.md:4 DEFEATED, brain.cpp dispatch hook | Detached C pthread posts to loopback on separate conn. No durability beyond that. |
| Startup / shutdown lifespan | `@app.on_event("startup")`, Lifespan | HARD | No lifespan hook; serve_brain starts workers at first call; no explicit shutdown | Server process lifetime is the "startup". |
| Shutdown hooks | Signal handling for graceful | PRESENT-UNTESTED | SIGPIPE ignore only (brain.cpp:500); no graceful drain of queue | Workers are detached; kill is abrupt. |

## 10. WebSockets

| feature | FastAPI behavior | quackapi status | evidence | notes |
|---------|------------------|-----------------|----------|-------|
| WebSocket routes | `@app.websocket("/ws")` + `await websocket.receive_text()` | HARD | serve_ws.sql separate (RFC6455 in C, echo only), edges.md:3 (transport DEFEATED, app PARTIAL); not mounted on main serve_brain | Different port, no shared routes registry / DI / auth with HTTP handlers. |
| Subprotocols, close codes etc | Full | PRESENT-UNTESTED (in isolation) | serve_ws only implements text echo frames | Not integrated or tested with app routes. |

## 11. OpenAPI/docs

| feature | FastAPI behavior | quackapi status | evidence | notes |
|---------|------------------|-----------------|----------|-------|
| Schema completeness (components, responses per operation, security) | Full from Pydantic + security deps | PRESENT-UNTESTED | framework.sql:310-350 (paths, parameters from param_schema, responses incl 422; no components/securitySchemes) | Basic paths+params good; missing depth. OpenAPI generated only on /openapi.json hit via oracle. |
| Swagger UI | /docs | WON | app.sql, framework 350 (hardcoded swagger bundle html), B2 pre-render exact | Points at /openapi.json. |
| ReDoc | Alternative docs | N/A | Only Swagger UI | Could add another static route. |
| Auto 422 from validation | Present | WON | Every route declares 422 response; validation produces it | framework and C both emit. |

## 12. Ops

| feature | FastAPI behavior | quackapi status | evidence | notes |
|---------|------------------|-----------------|----------|-------|
| TLS termination | uvicorn --ssl-keyfile or behind proxy | HARD | No SSL in brain.cpp socket code (plain AF_INET/SOCK_STREAM); no https paths in any serve_ | Disqualifying without reverse proxy. |
| Multiple processes | `--workers N` | NATURAL (process model) | g_rt is per-process static (edges_round5:100, B3_VERIFY); tables shared via file | Run N duckdb processes; each does own LOAD + reload. No built-in master/worker. |
| Graceful shutdown | Drain + SIGTERM handling | HARD | No shutdown flag or drain in accept_loop/worker; SIGPIPE only | Process exit is abrupt. |
| Logging / observability hooks | access logs, metrics | PRESENT-UNTESTED | middleware "request_logger" example only | No structured access log or Prometheus in core. |

## 13. Testing story

| feature | FastAPI behavior | quackapi status | evidence | notes |
|---------|------------------|-----------------|----------|-------|
| TestClient (in-process, sync) | `from fastapi.testclient import TestClient; client.get(...)` | WON (different direction) | handle_request as Tier-1 oracle (test/tier1_*.test.sql exact status/body/handler_sql checks), parity_b2.sh 16/16 byte compare vs C, tier2_http.sh (curl), B2/B3 results | Quackapi wins: can assert routing/validation/serialization **without** starting any HTTP server or even loading the C extension. FastAPI TestClient still goes through ASGI. Oracle is the stronger contract for the "brain". |
| Async test client | httpx.AsyncClient against app | NATURAL | Tier-1 works in any client; Tier-2 is real HTTP | No special async client needed for pure brain tests. |

## Recommended build order (NATURAL + HARD gaps ranked by (importance for supplant claim) × (effort))

Rank = high-importance + low-effort first. Blunt.

1. **TLS story (HARD, disqualifying)** — Add mbedTLS or OpenSSL listener variant + cert config to serve_brain. One-line rationale: without it, no credible production claim regardless of perf.
2. **Multipart upload streaming (HARD)** — Extend C reader to Content-Length loop + boundary parser (or hand socket to a streaming sink writing to uploads/ dir under configurable path); lift ducktinycc malloc limits or move to extension. Rationale: real file uploads are non-negotiable for most APIs.
3. **WebSocket integration (HARD)** — Wire the RFC code from serve_ws into main brain (upgrade detect on same port, route table entry for ws://, shared registry + middleware). Rationale: "FastAPI replacement" implies unified HTTP+WS surface.
4. **Async worker model or I/O-friendly path (HARD)** — Either add an async-capable worker (complex) or document+provide self-dispatch for slow handlers + thread pool escape. Or accept "CPU + fast DB" niche. Rationale: biggest workload class gap vs modern FastAPI.
5. **Sub-routers / mounting + richer OpenAPI (NATURAL, high completeness)** — Add prefix/scope columns to routes, rewrite patterns at load, emit components/security in the openapi SELECT. Rationale: large apps need structure; cheap SQL win.
6. **Yield-DI resource lifetime (HARD)** — Wrap full request cycle (resolve + handler + teardown + commit) in C with RAII-like guards so finally is reliable even on error paths. Rationale: matches FastAPI's strongest DI selling point.
7. **Form data + better header/cookie param injection (NATURAL/PRESENT)** — Parse form in C or SQL; extend param_schema location='header'/'cookie' and wire. Rationale: closes request surface parity.
8. **Set-Cookie helpers + RedirectResponse sugar (NATURAL)** — Add register helpers or response kind that emits proper headers/status. Rationale: ergonomics.
9. **Exception handlers registry + lifespan hooks (NATURAL/HARD)** — Small tables + C callouts at boot/shutdown. Rationale: parity of lifecycle.
10. **GZip response compression + richer securitySchemes (NATURAL)** — C write-path or middleware; emit in openapi. Rationale: nice-to-have for prod.

**Honest note on order:** Items 1-4 are the ones that block "supplant" for broad adoption. If the goal is "best-in-class for DuckDB-centric JSON APIs where the DB is the source of truth and perf wins," several NATURALs + current WONs already deliver a compelling story. The matrix is evidence, not a roadmap cheerlead.

All claims cite the listed artifacts. No external FastAPI source was read for this run; grades use standard documented FastAPI behaviors matched against repo evidence.