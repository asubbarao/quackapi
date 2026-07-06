# FastAPI DI, Testing, and Request Lifecycle — Pain Map for quackapi

**Research date:** 2026-07-02  
**Lens:** Dependency Injection, testing story, request lifecycle  
**Purpose:** Map FastAPI's DI pain to quackapi's design — what we beat, what we trade, what we owe

---

## 0. Reading key

Each pain point carries:
- **Frequency/severity** — how pervasive and blocking it is in practice  
- **Root cause** — the technical reason it hurts  
- **Classification** — (A) architectural crack quackapi beats; (B) execution gap (FastAPI could fix); (C) deliberate tradeoff to respect  
- **quackapi angle** — honest mapping of our model against this pain, with (a)/(b)/(c) DI-use-case breakdowns where relevant

---

## 1. `Depends()` is not a DI container — it's a function-scoped resolver

**Frequency/severity:** High/chronic. Every serious FastAPI codebase hits this; it's the #1 reason third-party libraries (`injector`, `python-dependency-injector`, `fastapi-injectable`) exist.

**Root cause:** `Depends()` is a decorator-time marker that FastAPI's request handler resolves inline per-request. There is no container object, no registry, no declarative lifetime configuration. Lifetimes — singleton, per-request, transient — have to be hand-implemented by the developer. The caching behavior (`use_cache=True` by default, deduplicated per request by identity of the factory function) is the only built-in scoping, and it only covers "call once per request." Anything crossing request boundaries is manual.

Concrete consequences:
- **Singleton state** requires module-level variables or `@lru_cache` on the factory. Tests that share module state between runs get bitten.
- **Transient (new instance every injection point)** requires `use_cache=False` — a footgun because forgetting it means two injection sites share a mutable object they didn't expect to share.
- **Startup-time DI is unsupported.** Lifespan/`@app.on_event("startup")` cannot resolve `Depends()`. Developers must construct the dependency graph manually in startup, then stash it in `app.state` or module globals, then reference it from a wrapper `Depends()`. Three-layer indirection for something a real container does natively.

**Classification:** (A) — architectural crack. The limitation is structural to "DI tied to a route handler invocation."

**quackapi angle:** We claim to sidestep this entirely because our "DI" is SQL-native. Let's be precise:
- **(a) Obviated:** The most common yield-dependency use case is injecting a database session. quackapi doesn't need this because the engine IS DuckDB — there is no session object to pass around. The query pipeline IS the session. This is a genuine and significant win.
- **(b) Solved differently:** Singleton configuration (API keys, rate limit settings, feature flags) maps to `CREATE SECRET` or config tables in DuckDB. They're available in every query without injection.
- **(c) Gap — application-level singletons:** If a quackapi application needs a connection pool to an external service (e.g., a Redis handle, a gRPC channel to an upstream), we have no DI mechanism to inject it per-request. Our model is SQL-first: if the external service is accessible via DuckDB extension (httpfs, postgres_scanner), we win. If it requires a Python/C++ object to be instantiated once and shared, we don't have an answer yet. **This is a documented edge.**

---

## 2. `yield` dependencies: teardown timing, exception swallowing, and the "can't modify response after yield" wall

**Frequency/severity:** High/blocking. Multiple open and historically-contentious GitHub issues.

**Root cause:** FastAPI's generator-based teardown pattern (`yield`-in-dependency) is evaluated by Starlette's `run_in_threadpool` / async generator machinery. Several pathological behaviors emerge:

### 2a. HTTPException doesn't reach yield `try/except`

`HTTPException` is caught and converted to a response by FastAPI's exception handler *before* teardown runs. The yield dependency sees it as `None` in its except clause — it cannot distinguish "route raised HTTPException(422)" from "route returned normally." A dependency that wanted to rollback on 4xx cannot.

**Root cause:** FastAPI's exception middleware wraps the full handler, converts `HTTPException` to a `JSONResponse`, and then starts teardown. The exception is *consumed* before teardown sees it.

**Workaround in use:** Unconditional `finally` blocks for rollback (always rollback), or passing a flag object into the dependency via `request.state`.

**Classification:** (B) — FastAPI-specific execution gap, not inherent to ASGI.

**quackapi angle:** (a) Obviated for DB session rollback — there is no session to roll back; DuckDB's per-query transactionality means a failed SQL statement simply doesn't commit. No teardown needed.

### 2b. Response is NOT sent to client until after-yield code completes

The user expectation is: yield = "response flies out, cleanup runs in background." Reality: FastAPI intentionally holds the TCP connection open until post-yield code finishes, because it needs to be able to upgrade the response to a 500 if teardown throws. Different HTTP clients expose this differently (curl closes aggressively; Postman/browsers wait). Result: slow cleanup code visibly blocks the client.

**Root cause:** Design choice in Starlette's `ServerErrorMiddleware` — a good choice for correctness, a bad surprise for performance.

**Classification:** (C) — deliberate tradeoff, hard to eliminate without sacrificing error surfacing.

**quackapi angle:** (a) Mostly obviated. Our model has no per-request teardown code. DuckDB's memory model cleans up per-query. If we add post-response hooks (audit logs, metrics flush), we need to make this a first-class concept and document that it behaves like `BackgroundTasks`, not like synchronous cleanup.

### 2c. Cannot set response headers/cookies after yield

Dependencies with yield cannot modify the `Response` object after yielding — the response is already serialized. Setting a header in teardown is a no-op or raises. This is particularly painful for session cookie rotation or tracing-ID propagation that needs to run unconditionally at the end of every request.

**Root cause:** Response object is finalized before teardown generators are exhausted.

**Classification:** (A) — architectural. The serialization order is baked into Starlette's response pipeline.

**quackapi angle:** (a) In our model, response headers are set by the SQL pipeline (`SELECT 'Set-Cookie' AS header_name, ...`). Post-yield mutation isn't a concept. But: if we want to add a tracing header based on query execution metadata (e.g., wall time, rows scanned), we need our C++ extension to inject headers after the SQL completes — this is a design affordance we should build explicitly.

---

## 3. Dependency trees: depth, opacity, and `Annotated` verbosity

**Frequency/severity:** Medium/chronic. Hits teams at scale; less relevant for small apps.

**Root cause:** FastAPI's dependency tree is implicitly defined through function signatures. A route that depends on `get_current_user` which depends on `get_db_session` which depends on `get_settings` creates a four-level chain that is invisible to tooling. The only way to see it is to read all the function signatures.

Adding `Annotated` (the modern form recommended since FastAPI 0.95+) helps type-checker integration but adds visual noise:
```python
CurrentUser = Annotated[User, Depends(get_current_user)]
```
Every new dependency type requires a new type alias. Developers either write verbose inline `Annotated[X, Depends(factory)]` or accumulate a module of type aliases — neither feels like a DI system.

**Sub-dependency resolution outside routes** is entirely absent from the framework. `Depends()` only resolves inside route decorators. Using a service class that needs a DB session in a CLI script, a scheduled job, or a Celery worker requires either manually constructing the entire dependency chain or switching to `fastapi-injectable`.

**Classification:** (A) for "no DI outside routes"; (B) for verbosity (addressable with better ergonomics).

**quackapi angle:**
- **(a) Obviated:** Our request model is `handle_request(method, path, headers, body) -> (status, headers, body)`. The SQL pipeline is self-contained. There is no "dependency that needs resolving" because the query IS the dependency graph — SQL's CTE structure is an explicit, readable, toolable dependency tree.
- **(b) Solved differently:** `CREATE AUTH` and `CREATE POLICY` replace auth dependency chains. A `SELECT * FROM current_user` in a CTE replaces `Depends(get_current_user)`.
- **(c) Gap:** If we ever support quackapi apps being called from non-HTTP contexts (CLI, batch), we need a story for "run this SQL outside of a request context." DuckDB makes this trivial — just execute the SQL against the same DB file — but we should document it as a first-class pattern rather than letting it be discovered.

---

## 4. Testing: `app.dependency_overrides` — global mutable state, async complexity, and DB rollback fragility

**Frequency/severity:** High/chronic. The #1 source of flaky FastAPI test suites according to multiple sources.

**Root cause:** `app.dependency_overrides` is a plain dict on the `FastAPI` application instance. It is global, mutable, and shared across all test functions in a process. The pattern is:
```python
app.dependency_overrides[get_db] = lambda: test_session
# ... test runs ...
app.dependency_overrides.clear()  # MUST not forget this
```

Problems:
1. **Leak between tests.** Forgetting `clear()` means overrides from test A silently infect test B. Symptoms: tests pass individually, fail in suite; false positives hiding real bugs. Identified as "#1 source of flaky suites."
2. **No scoping mechanism.** The dict has no awareness of test lifetimes, fixtures, or pytest's scope hierarchy. Developers must remember to wire up teardown in every fixture.
3. **Async test complexity.** Async SQLAlchemy 2.0 + pytest-asyncio requires savepoints (nested transactions) to roll back per-test. The correct pattern involves `BEGIN SAVEPOINT`, the test runs, `ROLLBACK TO SAVEPOINT`. Getting this right requires understanding async context managers, event loops per test vs per session, and SQLAlchemy's `AsyncSession.begin_nested()`. Three separate GitHub issues/discussions confirm this is routinely broken.
4. **TestClient wraps Starlette synchronously.** `TestClient` runs the ASGI app in a separate thread. `AsyncClient` (httpx) is required for true async behavior but requires `anyio`/`asyncio` mode in pytest. Two different clients for sync/async routes create test setup divergence.
5. **No way to verify override was actually invoked.** `dependency_overrides` is silent about whether the overridden dependency was called, called the right number of times, or was even registered against the right key (function identity matching is fragile with lambdas).

**Classification:** (A) for global-state leak (architectural to FastAPI's dict-based override); (B) for async test complexity (solvable with better tooling); (C) for inherent async complexity.

**quackapi angle:** This is a genuine design advantage for quackapi, but requires honest accounting.

- **(a) Obviated for DB state isolation:** DuckDB is the DB. A test can spin up an in-memory DuckDB instance per test — `duckdb.connect(':memory:')` — load the schema, and the entire DB vanishes when the test ends. No transactions to roll back, no savepoints, no async session management. This is fundamentally simpler.
- **(a) Obviated for auth override:** `CREATE AUTH` policies are SQL DDL on the DuckDB instance. Overriding auth in tests means running a different SQL setup — no global dict mutation, no shared app state.
- **(b) Different — handler testing:** Testing our `handle_request` oracle is a pure function call: `handle_request('GET', '/users/1', headers, body)` returns `(status, resp_headers, body)`. No HTTP client needed. No async machinery. No TestClient/AsyncClient split. This is a significant testability win and should be front-and-center in quackapi's pitch.
- **(c) Gap — integration tests against the C++ extension:** When testing the actual HTTP layer (the compiled extension serving real HTTP), we need a real server process and a real HTTP client. This is equivalent to FastAPI's `AsyncClient` tests. We don't improve on this layer — we just move it to a smaller, more stable surface.

---

## 5. `BackgroundTasks` — in-process, no retry, lost on crash, Depends() unavailable inside tasks

**Frequency/severity:** High/important. Every team that uses BackgroundTasks in production eventually hits one of these.

**Root cause:** `BackgroundTasks` is a list of callables run by Starlette after the response is sent, in the same process and event loop. Consequences:

1. **No dependency injection inside tasks.** Background task functions are plain callables, not route handlers. `Depends()` is not invoked. Services must be manually passed as arguments at scheduling time: `background_tasks.add_task(send_email, db=db, user=user)` — the `db` and `user` are captured at request time, not re-resolved when the task runs.

2. **No retry.** If the callable raises, the exception is logged and discarded. No backoff, no dead-letter queue, no visibility in a dashboard.

3. **No persistence.** Tasks live in memory. Process restart, pod eviction, OOM kill — all pending tasks vanish silently.

4. **Silent task discard bug (as of 2025, fix pending):** When a route returns a `Response` object that already has its own `background` attribute set, and the route also receives `BackgroundTasks` via `Depends()`, the injected tasks are silently discarded. Root cause: `routing.py` checks `if raw_response.background is None` before assigning — it doesn't merge. A `UserWarning` PR exists but the behavior itself is unresolved.

5. **Tasks block teardown:** Yield-dependency teardown runs AFTER background tasks, not before. If teardown releases a DB connection and a background task still holds a reference to the session object, you get use-after-free errors on the session.

**Classification:** (A) for no persistence/retry (not a web framework's job to be a job queue); (C) for in-process semantics (deliberate for simplicity).

**quackapi angle:**
- **(a) Obviated in philosophy:** quackapi is explicitly stateless and one-shot. We don't try to be a job queue. The clean answer is: if you need retries and persistence, use a real queue (Temporal, Celery, etc.). This is actually stronger than FastAPI's answer because FastAPI's `BackgroundTasks` creates a false sense of reliability.
- **(c) Our own gap here:** If a quackapi handler needs to fire a truly post-response side effect (audit log, webhook), we need a model for this. Options: (1) the C++ extension runs a detached coroutine after sending the response — same risks as BackgroundTasks; (2) the SQL pipeline writes to an outbox table, a separate worker drains it — better reliability. We should document option (2) as the quackapi-native pattern.

---

## 6. Middleware: reversed stack ordering, `request.state` fragility, and the `Depends`-vs-middleware split

**Frequency/severity:** Medium/surprising. Hits teams on initial setup; rarely blocks production.

**Root cause:**

1. **Reversed ordering.** Middleware registered with `app.add_middleware()` is applied outermost-last — the last `add_middleware()` call becomes the outermost wrapper. On the response path, it runs first. This is unintuitive: reading the code top-to-bottom, the last `add_middleware` is the first to see requests. CORS middleware placed in the wrong order causes preflight failures that look like a network problem.

2. **`request.state` is an unserialized namespace.** Middleware writes values to `request.state`; route handlers or dependencies read them. This is implicit coupling — there's no type declaration that connects the writer to the reader. In large codebases, `request.state.user` might be set by one of three middleware layers, and removing one breaks downstream readers with an `AttributeError` at runtime, not at type-check time.

3. **Yield-dependency teardown runs AFTER middleware response path.** This creates a subtle interaction: a middleware that reads `request.state` on the response path runs before yield-dependencies finish. If a dependency's teardown writes to `request.state` (e.g., to signal that the DB was committed), middleware cannot see it.

4. **Middleware cannot use `Depends()`.** Middleware is a pure ASGI layer; it does not participate in FastAPI's dependency resolution. To share logic between middleware and dependencies, developers either duplicate code or use `request.state` as a side channel — both are fragile.

**Classification:** (A) for Depends-vs-middleware split (structural); (B) for reversed ordering surprise (documentation/ergonomics gap); (C) for `request.state` runtime typing (inherent to Python's duck-typing).

**quackapi angle:**
- **(a) Our model:** Middleware is just SQL. Auth, rate limiting, request validation — all run as SQL expressions before (or within) the handler CTE chain. The "dependency vs middleware" split doesn't exist: everything is a CTE or a `WITH` clause. No implicit `request.state`; all context is passed as SQL columns down the pipeline.
- **(b) Solved:** CORS, tracing headers, rate limit headers — these are output columns of the SQL pipeline. Order is explicit (CTE evaluation order).
- **(c) Gap — true ASGI-layer work:** Connection-level concerns (TLS termination, HTTP/2 push, WebSocket upgrade) cannot be expressed as SQL. These live in the C++ extension layer. We should document the boundary: "SQL pipeline handles application logic; C++ layer handles protocol-level concerns."

---

## 7. Deep `Annotated` verbosity and typing fragility

**Frequency/severity:** Low-medium / growing. Became significant post-FastAPI 0.95 when `Annotated` became the recommended pattern.

**Root cause:** Modern FastAPI recommends:
```python
UserDep = Annotated[User, Depends(get_current_user)]
DBSessionDep = Annotated[AsyncSession, Depends(get_db)]
```
Every new dependency type needs a new alias. These aliases must be imported everywhere they're used — creating a `deps.py` module that becomes a secondary centralization point (ironic given that `Depends()` was supposed to avoid centralization).

Additionally, PEP 695 type alias syntax (`type UserDep = Annotated[...]`) interacts incorrectly with FastAPI's introspection in some versions, causing FastAPI to interpret the dependency as a query parameter instead.

`Depends` in `Annotated` uses function identity for cache deduplication. If `get_db` is imported from two different paths (common in large projects with re-exports), FastAPI treats them as different dependencies and calls the factory twice, creating two sessions. Silent double-session bugs are hard to trace.

**Classification:** (B) — FastAPI execution gap, mitigable with tooling/conventions.

**quackapi angle:** (a) Not applicable. Our DI is SQL; there are no type annotations to compose. Our "schema" for what a handler receives is declared via `CREATE ENDPOINT` DDL, not Python type hints. No `Annotated` verbosity.

---

## 8. DI-use-case map: (a) obviated / (b) different / (c) gap

This is the load-bearing output. FastAPI DI use-cases, classified against quackapi's model:

### (a) Obviated — quackapi natively eliminates the need

| Use case | FastAPI pattern | Why quackapi doesn't need it |
|---|---|---|
| DB session per request | `yield db_session` | DuckDB IS the DB; every query gets a fresh execution context |
| DB transaction rollback on error | `try/except/finally` in yield dep | Failed SQL simply doesn't commit; no session to roll back |
| Auth token validation | `Depends(get_current_user)` | `CREATE AUTH` — SQL DDL, runs before route logic |
| Policy enforcement | `Depends(check_permission)` | `CREATE POLICY` — SQL DDL, composable with auth |
| Config/settings injection | `Depends(get_settings)` | DuckDB `CREATE SECRET` / config table, available to every query |
| Request validation | Pydantic models via type hints | SQL constraints / CHECK expressions |
| Test DB isolation | `dependency_overrides` + savepoints | Spin up `:memory:` DuckDB per test; no shared state |
| Test auth override | `dependency_overrides[get_user]` | Different SQL setup per test; no global dict |

### (b) Solved differently — quackapi has an answer, it's just not FastAPI's

| Use case | FastAPI pattern | quackapi pattern |
|---|---|---|
| Per-request tracing ID | Middleware → `request.state.trace_id` | `SELECT uuid()` at start of pipeline; thread as CTE column |
| Rate limiting | Middleware or Depends | SQL against a rate-limit table; C++ extension enforces |
| Response headers from middleware | Middleware → `response.headers` | SQL output columns; C++ extension sets headers |
| CORS | `CORSMiddleware` | C++ extension handles preflight; configurable via SQL DDL |
| Audit logging | Yield dep teardown or BackgroundTasks | Write to outbox table in same SQL pipeline |

### (c) Genuine gaps — we cannot do this yet, must document

| Use case | FastAPI can do it? | quackapi status |
|---|---|---|
| Inject a persistent connection (Redis, gRPC channel) | Yes, via `@lru_cache` factory | **No.** SQL can't hold a live connection object. Must live in C++ extension as a global. We need a `CREATE CONNECTION` DDL concept. |
| Post-response side effects with retry | Partially (BackgroundTasks, no retry) | **No.** We recommend the outbox pattern (write to a table, drain separately). Needs explicit documentation. |
| Startup-time DI (lifespan) | Yes, via lifespan + `app.state` | **Partial.** DuckDB runs migrations/setup SQL in the startup path. Object-level resources (file handles, connection pools) live in C++ startup code. Document the boundary. |
| OAuth2 / OIDC callback flow with session cookie | Yes, complex but doable | **No native story.** Redirects + cookie setting are response-level. SQL can emit the Set-Cookie header value; the OAuth state machine needs either a DuckDB table or an external service. |
| WebSocket connection with stateful protocol | Yes, via WebSocket route | **No.** Our model is request/response. WebSocket is a documented non-goal. |
| Streaming responses (SSE, chunked) | Yes | **No.** Also a documented non-goal (one-shot model). |

---

## 9. Synthesis: where our model is strongest and where it's weakest

**Strongest:** Stateless, data-only APIs. The classic CRUD-over-relational-data use case is where quackapi eliminates the most FastAPI pain. No session management, no DI boilerplate, pure-function testability, trivially parallel execution.

**Second strongest:** Testing. The in-memory DuckDB test model is categorically simpler than the async SQLAlchemy savepoint dance. This should be a headline feature in our pitch.

**Weakest:** Stateful external resources. Any time a request handler needs a live connection to something outside DuckDB (Redis, a gRPC upstream, a mutable in-memory cache), we have no answer. The C++ extension can hold globals, but we have no SQL-accessible DI mechanism to inject them into the handler pipeline per-request. This is **the DI gap that matters most** and should drive the `CREATE CONNECTION` roadmap item.

**Honest reframe on "DI is our hardest open edge":** The framing in our project notes is slightly off. DI for DB sessions is easy — we solve it by elimination. DI for external resources IS hard and remains unsolved. We should stop saying "DI is hard" generically and start saying "connection-injection for external stateful resources is our open problem."

---

## Sources

- [Better Dependency Injection in FastAPI — Vlad Iliescu](https://vladiliescu.net/better-dependency-injection-in-fastapi/)
- [Dependency Injection in Python, Beyond FastAPI's Depends — Guillaume Launay](https://medium.com/@guillaume.launay/dependency-injection-in-python-beyond-fastapis-depends-eec237b1327b)
- [Testing FastAPI Applications — OddBird](https://www.oddbird.net/2024/02/09/testing-fastapi/)
- [FastAPI: Yield dependency doesn't get HTTP exceptions — GitHub Issue #869](https://github.com/fastapi/fastapi/issues/869)
- [Injected BackgroundTasks silently discarded — GitHub Issue #15111](https://github.com/fastapi/fastapi/issues/15111)
- [Yield dependency response timing — GitHub Discussion #10004](https://github.com/fastapi/fastapi/discussions/10004)
- [Why doesn't FastAPI support DI in startup — GitHub Discussion #12082](https://github.com/fastapi/fastapi/discussions/12082)
- [Accessing dependencies in background tasks — GitHub Issue #4956](https://github.com/fastapi/fastapi/issues/4956)
- [Database session as a dependency and commit/rollback pattern — Upesh Jindal](https://medium.com/@upesh.jindal/database-session-as-a-dependency-and-commit-rollback-pattern-f5533b2667e0)
- [app.dependency_overrides testing guide — hrekov.com](https://hrekov.com/blog/testing-fastapi-dependency-injection)
- [FastAPI BackgroundTasks limitations — dev.to/richard_quaicoe](https://dev.to/richard_quaicoe_2398278be/managing-background-tasks-in-fastapi-from-basic-to-production-ready-beyond-fire-and-forget-ddm)
- [Middleware ordering CORS problem — Medium/saurabhbatham](https://medium.com/@saurabhbatham17/navigating-middleware-ordering-in-fastapi-a-cors-dilemma-8be88ab2ee7b)
- [fastapi-injectable: DI outside route handlers](https://github.com/JasperSui/fastapi-injectable)
- [FastAPI Injectable documentation](https://fastapi-injectable.readthedocs.io/)
- [Nested transactions for testing — SQLAlchemy Discussion #11658](https://github.com/sqlalchemy/sqlalchemy/discussions/11658)
