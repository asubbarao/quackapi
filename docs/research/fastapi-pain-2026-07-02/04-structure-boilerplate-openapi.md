# FastAPI Pain Points: Structure, Boilerplate & OpenAPI

**Research date:** 2026-07-02  
**Lens:** Project structure, CRUD boilerplate, lifespan/state, OpenAPI ergonomics, DI at scale, config/versioning  
**Purpose:** Informing quackapi positioning — where "routes/models as introspectable SQL tables" wins vs. where it's a tradeoff

---

## 1. Project Structure Chaos — No Opinionated Layout

### What It Is
FastAPI ships zero project scaffolding. Every team reinvents a directory structure. The official docs' "Bigger Applications" tutorial shows one flat router include pattern that breaks down around 10+ modules. The most-starred community guide ([zhanymkanov/fastapi-best-practices](https://github.com/zhanymkanov/fastapi-best-practices), **17.6k stars**) exists precisely to compensate for this gap.

### Evidence
- Dozens of competing cookiecutter templates on GitHub (top ones: 354–603 stars each), no canonical answer
- Dev.to post "How I structure big FastAPI projects" explicitly opens: "Writing everything in one or two files is not exactly ideal" — the author borrowed SvelteKit's file-based routing concept because FastAPI provides none
- Discussion [fastapi#6860](https://github.com/fastapi/fastapi/discussions/6860): "Get the app in APIRouter without circular imports" — a recurring shape: teams discover circular imports when routers try to reference the app, because there's no framework-enforced boundary
- Community consistently bifurcates between **by-layer** (`routers/`, `models/`, `schemas/`, `crud/`) and **by-domain** (`users/`, `orders/`) — neither is blessed; many projects start with by-layer and regret it at scale (Netflix's Dispatch pattern cited as the rescue)

### Root Cause
FastAPI is explicitly "un-opinionated" at the directory level. This is a **deliberate tradeoff** (see §C below) but it creates a cold-start tax for every project.

### Classification
**(A) Architectural crack + (C) deliberate tradeoff.**

### quackapi Angle
If quackapi routes are registered as `INSERT INTO routes` or `CREATE ROUTE`, the "project structure" question partially dissolves — the app's structure *is* its database schema, which is universally introspectable:

```sql
SELECT module, tag, path, method, handler_ref
FROM routes
ORDER BY module, path;
```

No directory layout encodes this. A new engineer can query the running system to get a full map of the app. Circular imports become impossible because routes and their modules are data, not Python import-time side effects. The question "where does this route live?" becomes a SELECT, not a grep through a directory tree.

**Gap:** quackapi needs a migration/scaffolding story for *initial* structure. But "structure" as an ongoing concern is solved by SQL.

---

## 2. CRUD Boilerplate — Model Proliferation

### What It Is
FastAPI's own documentation ([Extra Models](https://fastapi.tiangolo.com/tutorial/extra-models/)) explicitly recommends **at least 3 Pydantic models per entity**: `UserIn` (with password), `UserOut` (no password), `UserInDB` (hashed password). In practice this grows:

| Model | Purpose |
|-------|---------|
| `UserBase` | Shared fields |
| `UserCreate` | POST input |
| `UserUpdate` | PATCH input (all optional) |
| `UserResponse` | GET output |
| `UserInDB` | ORM / storage shape |
| `User` (SQLAlchemy) | Database table |

Six declarations for one entity. This spawned an entire ecosystem:
- **SQLModel** (by FastAPI's own creator) — unifies SQLAlchemy + Pydantic into one class to eliminate duplication
- **fastapi-crudrouter** — auto-generates CRUD routes from Pydantic models
- **fastapi-users** — pre-built user auth + CRUD
- **FastCRUD** — composable CRUD layer
- **benavlabs/FastAPI-boilerplate** (603 stars) — exists to give you a working CRUD scaffold out of the box

### Evidence
- FastAPI docs state: "Reducing code duplication increases the chances of bugs, security issues, and code desynchronization issues" — then immediately describe a pattern requiring 3+ models
- The Pydantic `UserBase`→`UserIn`/`UserOut`/`UserInDB` inheritance pattern is reproduced verbatim in at least 15 independent tutorials, indicating it's the only widely known solution
- [pydantic#7371](https://github.com/pydantic/pydantic/discussions/7371): "Using a same model for both data and API" — user asks if it's possible; answer is complicated
- SQLModel's own homepage opens with: "The motivation addresses the problem of juggling between SQLAlchemy models, Pydantic schemas, and the inevitable duplication"

### Root Cause
FastAPI treats API input validation, API output filtering, and ORM schema as three independent concerns — correct architecturally, painful practically. Each model is a class, and Python classes don't compose or diff easily.

### Classification
**(A) Architectural crack quackapi wins.** This is the most direct win.

### quackapi Angle
In quackapi, the entity's schema *is* a table schema + a `param_schema` registration:

```sql
CREATE TABLE users (
    id    BIGINT PRIMARY KEY,
    email VARCHAR NOT NULL,
    -- hashed_password stays here, never in response
);

CREATE ROUTE POST '/users'
    INPUT  (email TEXT, password TEXT)  -- validated at boundary
    OUTPUT SELECT id, email FROM users WHERE id = :new_id;
```

There's no separate UserIn/UserOut/UserInDB. The output projection is a SELECT clause — a field-level filter, not a separate class. Schema changes cascade from DDL, not from synchronizing six Python classes. OpenAPI generation is a `SELECT` over the route registry + param_schema; the schema is always consistent with reality because it *is* the reality.

**This directly eliminates the entire SQLModel/fastapi-crudrouter reason to exist.**

---

## 3. Lifespan / Startup-Shutdown — Silent Failures & Router Scoping

### What It Is
FastAPI deprecated `@app.on_event("startup")` / `@app.on_event("shutdown")` in favor of a `lifespan` context manager. The migration introduced two serious failure modes:

**A) Silent failure when mixing old and new:**  
[Discussion #9604](https://github.com/fastapi/fastapi/discussions/9604): When `lifespan=` is passed to the `FastAPI()` constructor, the deprecated `@app.on_event` handlers **silently stop executing** — no warning, no error. One commenter spent an hour debugging; third-party libraries relying on startup events "started silently failing." FastAPI's collaborator acknowledged: "The best thing to do is to not have it silent. Starlette should solve this."

**B) Lifespan doesn't work on `APIRouter`:**  
[Discussion #10464](https://github.com/fastapi/fastapi/discussions/10464): `lifespan` context manager only works on the root `FastAPI()` app, not on `APIRouter`. The old `on_event` *did* work on routers. This forces all startup/shutdown logic to be centralized in `main.py`, which breaks the modularity that `APIRouter` is supposed to provide. Status: "Not supported yet" at the Starlette level, "will be supported eventually."

**C) Sharing state across routers:**  
The only idiomatic way to share startup-initialized resources (DB pools, ML models, HTTP clients) across multiple routers is either: (a) module-level global variables (FastAPI's own docs suggest `app.state`), or (b) wrapping them in `Depends()` functions that re-initialize per-request if not careful. The `app.state` approach requires importing `app` from `main.py`, which triggers circular imports (see §1).

### Evidence
- Discussion #9604 has 40+ upvotes; the silent-fail behavior was present for multiple minor versions
- Discussion #10464: developer explicitly notes "it worked before with on_event" — regression in migration
- FastAPI docs on lifespan acknowledge: "Doing that in separate functions that don't share logic or variables together is more difficult as you would need to store values in global variables or similar tricks"

### Root Cause
Starlette (FastAPI's underlying framework) owns the lifespan mechanism. FastAPI's deprecation moved faster than Starlette's feature parity.

### Classification
**(A) Architectural crack quackapi wins** (startup/shutdown is a DuckDB server concern, not the app's) + **(B) execution gap** (the silent-fail behavior is just a bug).

### quackapi Angle
In quackapi, the "app" is a DuckDB database that loads at server start. "Startup" is: attach extensions, register secrets, create sessions table. These are SQL statements in `init.sql`, not Python context managers. "Shared state across routers" is just... shared tables:

```sql
-- shared connection pool = DuckDB's own connection pool
-- shared ML model = a loaded extension or a table of embeddings
-- shared config = SELECT * FROM app_config WHERE key = 'model_path'
```

No module-level globals. No lifespan scoping confusion. No `app.state` circular import. The database *is* the shared state.

---

## 4. OpenAPI Edge Cases

### 4a. Auto-Generated operationId Ugliness

Default FastAPI `operationId` format: `{function_name}_{path}_{method}` → produces monstrosities like:

```
Body_pdf_parse_tables_api_v0_parse_tables_pdf__pdf_type___parser___orient__post
```

This breaks client code generation (openapi-generator, Speakeasy, Kiota) because generated function names become unreadable. FastAPI's own "advanced path operation configuration" docs acknowledge the problem and offer a `generate_unique_id_function` escape hatch. Third-party `fastapi-utils` package provides `simplify_operation_ids()` as a common fix. [Discussion #7448](https://github.com/fastapi/fastapi/discussions/7448) covers the auto-generated body schema naming problem.

**Classification:** **(A) quackapi wins.** Operation IDs in quackapi are the route names — declared explicitly in DDL. They're whatever you write in `CREATE ROUTE 'get_user'`. No auto-generation, no ugliness.

### 4b. Union / Discriminated Union Rendering (anyOf vs oneOf)

[Discussion #8504](https://github.com/fastapi/fastapi/discussions/8504): FastAPI generated `anyOf` for discriminated unions when OpenAPI spec requires `oneOf`. Client code generators (openapi-generator specifically named) produced incorrect output. Root cause: Pydantic's schema generator, not FastAPI. Fixed eventually, but only after affecting production client libraries.

[pydantic#7491](https://github.com/pydantic/pydantic/issues/7491) + [pydantic#5436](https://github.com/pydantic/pydantic/issues/5436): Nested discriminated unions remain broken in OpenAPI schema generation as of Pydantic v2. The discriminator assumption is flat mapping; nested unions break the schema.

[Issue #5232](https://github.com/fastapi/fastapi/issues/5232): `response_model` with a discriminated union doesn't validate correctly — FastAPI validates against each union member individually rather than using the discriminator.

**Classification:** **(B) Execution gap.** These are known bugs, fixed or in progress. quackapi angle: OpenAPI is a SELECT over `routes` + `param_schema` — the schema is generated from the actual parameter declarations, not from Python type introspection. There's no anyOf vs oneOf ambiguity because the schema is explicit SQL DDL.

### 4c. Large Schema Performance

FastAPI generates the entire OpenAPI JSON document on first `/openapi.json` request. For apps with 200+ routes and complex nested models, this causes: (a) first-hit latency spike, (b) Swagger UI rendering slowness (all schemas loaded upfront). The standard advice is to disable docs in prod (`openapi_url=None`). 

**quackapi angle:** OpenAPI is `SELECT ... FROM routes JOIN param_schema`. DuckDB executes this in microseconds regardless of route count. Can be pre-materialized as a Parquet file. Swagger UI performance is a frontend concern, not affected.

### 4d. Auth Schemes in Swagger UI

Setting up Bearer/OAuth2 flows in Swagger UI requires correct `securitySchemes` configuration in `openapi_extra` or through FastAPI's `security=` parameters. The interaction between `OAuth2PasswordBearer`, `HTTPBearer`, and the actual Swagger "Authorize" button has numerous gotcha issues (the lock icon appears but doesn't attach the header, schemes don't cascade to all routes, etc.). A well-traveled Stack Overflow question has 100k+ views.

**quackapi angle:** Auth scheme is a row in `security_schemes` table. The Swagger UI config is generated from that table. No class-level decoration required.

---

## 5. Dependency Injection at Scale — Depends() Pain

### What It Is
FastAPI's `Depends()` works elegantly for simple cases: inject a DB session, check auth. It breaks down at scale along three axes:

**A) Can't use Depends() outside routes:**  
[j-sui.com/2024: "Easily Reusing Depends Outside FastAPI Routes"](https://j-sui.com/2024/10/26/use-fastapi-depends-outside-fastapi-routes-en/) — if business logic lives inside `Depends()` factories, that logic is inaccessible from CLI scripts, background workers, Celery tasks, or tests that don't spin up the full app. Package `fastapi-injectable` was created specifically to solve this. The "outside routes" problem is a known, acknowledged limitation in [vladiliescu.net's "Better DI in FastAPI"](https://vladiliescu.net/better-dependency-injection-in-fastapi/).

**B) Deep chains are hellish:**  
`A → B → C → D → E → F` means six `Depends()` factory functions, each manually declaring its sub-dependencies. "Implementing different instance lifetimes (singletons, per-request, transient) with FastAPI's Depends system can be extremely challenging" — managing this requires `dependency-injector` or `injector` + `fastapi-injector`.

**C) App factory pattern + include_router is slow:**  
[Discussion #6302](https://github.com/fastapi/fastapi/discussions/6302): `include_router` does "a bunch of introspection and checks for each route" — expensive at import time. One team reduced test suite from **2 minutes to 15 seconds** by bypassing `include_router` and directly assigning the router. Creating a fresh app per test (the correct isolation pattern) amplifies this.

**D) `response_model` double validation:**  
[fastapi#1359](https://github.com/fastapi/fastapi/issues/1359) + discussion [#10954](https://github.com/fastapi/fastapi/discussions/10954): FastAPI validates responses through Pydantic even when the return type is already a Pydantic model. This creates double validation: once when you build the model, once when FastAPI serializes it. [fastapi-mistakes-that-kill-your-performance](https://dev.to/igorbenav/fastapi-mistakes-that-kill-your-performance-2b8k) quantifies: "20-50% overhead to response processing." Pydantic object creation is 6.5x slower than dataclasses; 2.5x higher memory.

### Root Cause
`Depends()` is a Python decorator that only the ASGI framework knows how to resolve. It's not a general DI container. This is a fundamental design choice — simple to learn, limited at scale.

### Classification
**(A) + (B).** Both architectural crack and execution gap.

### quackapi Angle
In quackapi, "dependencies" are SQL JOINs and CTEs — composable, reusable, and introspectable. There's no `Depends()` chain because the request handler IS a query:

```sql
CREATE ROUTE GET '/orders/:id'
AS
  WITH auth AS (SELECT * FROM auth_context(:token)),
       order AS (SELECT * FROM orders WHERE id = :id AND banner_id = auth.banner_id)
  SELECT o.*, u.email FROM order o JOIN users u ON o.user_id = u.id;
```

No factory proliferation. No singleton vs per-request confusion. No test isolation pain because SQL is idempotent. The "can't use Depends outside routes" problem doesn't exist — the query is a string, callable from anywhere.

---

## 6. Config/Env Management — pydantic-settings Migration & App Factory Friction

### What It Is

**A) pydantic-settings v1→v2 migration:**  
`pydantic-settings` was extracted from pydantic-core as a separate package in Pydantic v2. The migration requires:
- Replace `from pydantic import BaseSettings` → `from pydantic_settings import BaseSettings`
- Replace `class Config:` inner class → `model_config = SettingsConfigDict(...)`
- Re-test all validators (stricter coercion in v2 silently breaks env var parsing)

FastAPI 0.126.0–0.128.0 dropped Pydantic v1 support in three rapid releases, forcing rapid migration. [fastapi#9709](https://github.com/fastapi/fastapi/discussions/9709) collected widespread reports. Teams running `validator`/`root_validator` decorators (deprecated in v2) had **silent failures** — the decorators were recognized but did nothing until the explicit deprecation warning cycle completed.

**B) App factory pattern:**  
The canonical test isolation pattern is an app factory function:
```python
def create_app(settings: Settings) -> FastAPI:
    app = FastAPI()
    app.include_router(users_router)
    return app
```
This correctly avoids import-time side effects. But `include_router` is expensive (see §5C). And settings injection requires `app.dependency_overrides` for tests, which is documented as "complicated and sometimes flaky."

**C) API versioning — no built-in story:**  
FastAPI has no native API versioning. Options: (1) prefix all routes manually (`/v1/`, `/v2/`), (2) mount sub-applications, (3) third-party `fastapi-versionizer` or `fastapi-easy-versioning`. Mounting a v1 sub-app and a v2 sub-app causes Starlette's greedy path matching to produce `405 Method Not Allowed` on overlap — [documented 2025 issue](https://www.johal.in/fastapi-router-prefixed-sub-app-mounting-2025/).

**D) StaticFiles can't be mounted on APIRouter:**  
[Discussion #9070](https://github.com/fastapi/fastapi/discussions/9070): `APIRouter.mount()` silently ignores `StaticFiles` sub-applications. `include_router()` only transfers `Route`, `APIRoute`, and `WebSocketRoute` — `Mount` routes are dropped. Documentation implies `APIRouter` supports "all the same options" as `FastAPI`, which is false for sub-applications.

### Root Cause
FastAPI inherits Starlette's routing model, where sub-applications and routes are different categories. `APIRouter` is a route collector, not a full ASGI app. The mismatch between the conceptual model (APIRouter ≈ mini-app) and the implementation (APIRouter = route list) leaks through repeatedly.

### Classification
**(A) Architectural crack** for versioning and sub-app mounting. **(B) Execution gap** for pydantic-settings silent failures.

### quackapi Angle
Config in quackapi is a table:
```sql
SELECT value FROM app_config WHERE key = 'db_url';
```
No `BaseSettings`, no env var coercion, no migration pain. API versioning is route metadata:
```sql
SELECT * FROM routes WHERE version = 'v2';
-- v1 routes coexist; routing table is a join condition
```
No sub-app mounting. No Starlette greedy path matching. StaticFiles serving is a separate concern (the C++ server handles it) — not mixed into the routing layer.

---

## Cross-Cutting: "Magic" and Import-Time Errors

A recurring but hard-to-cite class of complaints: FastAPI's heavy use of Python decorator magic and type introspection means errors surface at import time with cryptic messages, not at call time with clear context. Examples:
- A typo in a `Depends()` signature throws `TypeError` at server start, not at the route call
- Response model incompatibility raises `pydantic.ValidationError` deep in FastAPI internals with a stacktrace that points to FastAPI internals, not user code
- `@app.on_event` silently no-ops when `lifespan=` is also set (§3A)
- `APIRouter.mount(StaticFiles(...))` silently drops the mount (§6D)

The common thread: **FastAPI's implicit behaviors fail silently or with misleading errors**. The framework does a lot "for you" but provides little introspective tooling to verify what it actually did.

**quackapi angle:** The app's route registry *is* inspectable at any time:
```sql
SELECT path, method, handler_ref, is_active, param_schema
FROM routes
WHERE is_active = FALSE;  -- anything that failed to register shows up here
```
No implicit behaviors. The framework state is a database — queryable, auditable, debuggable.

---

## Summary: Classification Matrix

| Pain Point | Class | quackapi Win? | Notes |
|---|---|---|---|
| No opinionated project structure | A+C | **Yes** — structure = schema | Tradeoff: cold-start scaffold story needed |
| CRUD model proliferation (5+ models/entity) | A | **Strong yes** — OUTPUT = SELECT | Biggest, clearest win |
| Lifespan/router state sharing | A+B | **Yes** — state = DuckDB tables | DuckDB init.sql replaces lifespan |
| on_event silent failure on lifespan mix | B | Inherits from above | Bug, not architecture |
| operationId ugliness | A | **Yes** — IDs are DDL names | Explicit, not auto-generated |
| Union anyOf/oneOf rendering | B | **Yes** — schema is explicit DDL | No Python type introspection |
| Depends() at scale / outside routes | A | **Yes** — deps = CTEs | No factory proliferation |
| include_router slow in tests | B | **N/A** — no include step | SQL routes registered atomically |
| response_model double validation | B | **Yes** — no validation layer wrapping | OUTPUT projection is the contract |
| pydantic-settings v2 migration | B | **N/A** — no Pydantic | No migration surface |
| API versioning — no built-in | A+C | **Yes** — version = route field | SQL filter handles versioning |
| StaticFiles can't mount on APIRouter | B | **N/A** — C++ server handles static | Clean separation |

---

## Key Sources

- [zhanymkanov/fastapi-best-practices](https://github.com/zhanymkanov/fastapi-best-practices) — 17.6k stars; primary community consensus doc
- [fastapi#6860](https://github.com/fastapi/fastapi/discussions/6860) — circular import via APIRouter
- [fastapi#9604](https://github.com/fastapi/fastapi/discussions/9604) — lifespan + on_event silent failure
- [fastapi#10464](https://github.com/fastapi/fastapi/discussions/10464) — lifespan not working on APIRouter
- [fastapi#9070](https://github.com/fastapi/fastapi/discussions/9070) — StaticFiles silent drop on APIRouter
- [fastapi#8504](https://github.com/fastapi/fastapi/discussions/8504) — anyOf vs oneOf union schema
- [fastapi#6302](https://github.com/fastapi/fastapi/discussions/6302) — include_router slow, 2min→15s test speedup
- [fastapi#5393](https://github.com/fastapi/fastapi/discussions/5393) — response_model validation overhead debate
- [fastapi#1359](https://github.com/fastapi/fastapi/issues/1359) — response validation heavier than expected
- [pydantic#7491](https://github.com/pydantic/pydantic/issues/7491) + [#5436](https://github.com/pydantic/pydantic/issues/5436) — nested discriminated unions broken
- [vladiliescu.net — Better DI in FastAPI](https://vladiliescu.net/better-dependency-injection-in-fastapi/) — Depends() at scale critique
- [fastapi-best-practices: async/concurrency gotchas](https://github.com/zhanymkanov/fastapi-best-practices)
- [fastapi-mistakes-that-kill-your-performance](https://dev.to/igorbenav/fastapi-mistakes-that-kill-your-performance-2b8k)
- [fastapi#9709](https://github.com/fastapi/fastapi/discussions/9709) — pydantic v2 migration wave
- [SQLModel motivation](https://sqlmodel.tiangolo.com/tutorial/fastapi/) — creator's explicit rationale for eliminating duplication
