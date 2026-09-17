# FastAPI developer-experience demand — 2026-09-07

This is a prioritization for QuackAPI, not a claim of a popularity survey. The
ordering weighs FastAPI's maintained roadmap, explicit developer requests, the
size of the current QuackAPI gap, and whether the feature strengthens the
DuckDB-native model instead of copying Python abstractions literally. Sources
are FastAPI's official documentation and its own GitHub repository.

## 1. Declarative request/response middleware

**Why now.** FastAPI defines middleware as code that runs around every request
and response, including response-header changes and a documented nesting order.
Its current maintainer roadmap, opened October 2, 2023, separately calls out
router-level middleware and "Improved middleware support/API." An early request
for sharing cached dependencies between middleware and routes was opened July
23, 2019, and the interaction still creates lifecycle questions in real apps.

- [FastAPI middleware documentation](https://fastapi.tiangolo.com/tutorial/middleware/)
- [FastAPI roadmap #10370](https://github.com/fastapi/fastapi/issues/10370)
- [Middleware and dependencies #402](https://github.com/fastapi/fastapi/issues/402)

**Delivered QuackAPI capability.** QuackAPI now has database-scoped SQL
middleware. Its exact grammar, bindings, output protocol, bounds, and endpoint
scope are documented in [the middleware reference](MIDDLEWARE.md):

```sql
CREATE [OR REPLACE] MIDDLEWARE <name>
  BEFORE|AFTER [GROUP <group>]
  AS <single SQL statement>;
DROP MIDDLEWARE <name>;
```

The server binds request values as prepared-statement parameters, never by
interpolating SQL: `$method`, `$path`, `$query_json`, `$body`, `$client_ip`,
`$request_id`, `$route`, `$group`, `$auth_subject`, `$headers_json`, verified
`$claims_*`, plus `$status` and `$elapsed_ms` in `AFTER`. Result columns are a
small typed protocol:

| Column | Phase | Meaning |
|---|---|---|
| `header_name VARCHAR`, `header_value VARCHAR` | either | Add one response header per non-null pair. |
| `allow BOOLEAN`, `status INTEGER`, `body VARCHAR` | either | `allow=false` blocks with the supplied response; `allow=true` requires `status` and `body` to be NULL. |

Registration order is deterministic (creation order; a replace keeps its
position). Global hooks run before group hooks on the request path and after
them on the response path. A hook error, invalid result shape, invalid status,
or an incomplete header pair fails closed and prevents the handler from
running. Route matching, authentication, and policy rewriting run before
`BEFORE`; request validation occurs inside the handler afterwards. An
unauthenticated request therefore remains a 401/403 and no middleware can gain
a handler or claims it did not authorize. `AFTER` runs after the handler has
produced its response.

**DuckDB fit and risk.** This maps naturally to the existing DDL registry and
prepared route execution. It avoids an opaque C++ plugin API while supporting
request IDs, audit inserts, tenant gates, timing, and security headers. The
first release is HTTP-only, SQL-only, and cannot mutate request identity or
claims; built-in health, named/built-in GraphQL, and SSE keep their dedicated
paths and do not invoke SQL middleware yet.

**Acceptance tests.** The delivered SQLLogicTest coverage includes global/group
order, replacement position, matching and nonmatching groups, parameter values
containing quotes, multiple headers, rejection responses, malformed output,
hook SQL failure, unauthenticated protected routes, policy-filtered results,
and `quackapi_request`. Follow-on work should add TCP dispatch coverage and
explicit lifecycle cleanup coverage.

## 2. Promote the in-process TestClient and scoped test overrides

**Why now.** FastAPI treats `TestClient` plus ordinary assertions as its basic
testing workflow. Its documentation says to create the client from the app and
use it like HTTPX. Its roadmap explicitly lists improved dependency overrides
for testing. FastAPI issue [#1273](https://github.com/fastapi/fastapi/issues/1273),
opened April 16, 2020, requested an async-safe client because the client event
loop complicated background async work; the GitHub search result currently
shows 14 thumbs-up reactions. FastAPI's official override guide also shows that
overrides are global app state which must be reset after a test.

- [FastAPI testing documentation](https://fastapi.tiangolo.com/tutorial/testing/)
- [FastAPI testing overrides](https://fastapi.tiangolo.com/advanced/testing-dependencies/)
- [Async TestClient request #1273](https://github.com/fastapi/fastapi/issues/1273)

**Verified QuackAPI gap.** `src/quackapi_extension.cpp` already exposes
`quackapi_request` as a no-TCP "SQLLogic TestClient" and
`src/include/quackapi_server.hpp` calls it a test dispatch path. It is not part
of the public README function reference, has no documented isolation or
override scope, and offers response rows rather than first-class assertion
helpers.

**Recommended first slice.** Make `quackapi_request` a documented, stable
TestClient surface and add a scoped `quackapi_test_override` registry for route
dependencies introduced with middleware/lifespan work. Keep assertions plain
SQL in v1: return `status`, headers, bytes, decoded body, error, and request ID
without binding a port. Add optional `quackapi_assert_response` only if it can
produce useful SQLLogicTest failures without a mini assertion language.

**DuckDB fit and risk.** It is unusually strong for QuackAPI: tests execute in
the same ephemeral DuckDB transaction/catalog without TCP, Python, or a second
event loop. The key risk is leaking override state across tests, so overrides
must be pushed/popped by an RAII guard and rejected outside an explicit scope.

**Acceptance tests.** Test HTTP parity for method/path/query/body/headers,
protected routes, middleware, route replacement, response formats, parallel
test requests, nested override scopes, cleanup after a thrown SQL error, and
the guarantee that no listener is opened.

## 3. Output contracts: response schema, validation, filtering, and OpenAPI

**Why now.** FastAPI's response-model documentation says it validates returned
data, adds a response JSON Schema to OpenAPI for docs and client generation,
and filters undeclared fields for security. Its roadmap also prioritizes
Pydantic parsing and serialization. This is a clear contract developers expect,
not merely documentation polish.

- [FastAPI response models](https://fastapi.tiangolo.com/tutorial/response-model/)
- [FastAPI roadmap #10370](https://github.com/fastapi/fastapi/issues/10370)

**Verified QuackAPI gap.** The route state stores only `body_schema`;
`src/quackapi_server.cpp` validates that schema on request input. The public
`CREATE ROUTE` grammar documents `BODY SCHEMA` but no response schema, output
filtering, or output contract. OpenAPI is generated, but cannot express an
explicit output model beyond inferred handler columns.

**Recommended first slice.** Add `RESPONSE SCHEMA '<JSON Schema>'` to the
canonical route representation. Validate it at route creation; on JSON
responses validate each serializable row before sending, fail with 500 on an
invalid handler result, and emit the same schema in OpenAPI. Do **not** silently
strip fields in v1: validation catches accidental leaks while a later explicit
`RESPONSE ONLY (col, ...)` can provide database-native projection without
ambiguous JSON-Schema coercion.

**DuckDB fit and risk.** QuackAPI already uses JSON Schema for request bodies,
and DuckDB result metadata provides useful creation-time checks. Per-row output
validation costs CPU and must not be applied to CSV, Arrow, Parquet, HTML,
streaming, or explicit non-JSON responses. Make those unsupported combinations
DDL errors for the first release.

**Acceptance tests.** Verify OpenAPI output schema, missing/incorrect field
returns 500 before bytes are sent, nullable fields, multi-row arrays, non-JSON
format rejection, replacement atomicity, and a secret column rejected by the
contract.

## 4. Reusable parameter bundles and strict query/header/cookie input

**Why now.** FastAPI added Pydantic query-parameter models in 0.115.0 so related
parameters can be reused, validated, documented together, and configured to
reject unknown keys. The maintained roadmap names model support for `Query`,
`Form`, and related input locations.

- [FastAPI query parameter models](https://fastapi.tiangolo.com/tutorial/query-param-models/)
- [FastAPI roadmap #10370](https://github.com/fastapi/fastapi/issues/10370)

**Verified QuackAPI gap.** It supports individual `PARAM` declarations with
types, defaults, locations, and constraints, but no named reusable parameter
set or route-level unknown-parameter policy. The state has a flat
`vector<QuackapiParamSpec>` only.

**Recommended first slice.** Add `CREATE PARAMETER SET filters (...)` and
`CREATE ROUTE ... PARAMETER SET filters [FORBID EXTRA QUERY]`. Compile it into
the same immutable parameter representation used by `PARAM`, with duplicate
names and conflicting locations rejected before registry mutation.

**DuckDB fit and risk.** This is parser/registry work and reuses the existing
binding/422 validation path. Lists and deeply nested object models should wait;
their URL encoding and OpenAPI representation need a separate design.

**Acceptance tests.** Reuse one set across routes, override defaults only with
an explicit route option, reject duplicates, preserve OpenAPI entries, reject
unknown keys when configured, and keep unknown keys accepted by default for
compatibility.

## 5. Explicit server lifecycle hooks

**Why later.** FastAPI recommends lifespan context managers for startup and
shutdown; its documentation gives resource initialization before `yield` and
cleanup after requests drain. The roadmap still lists improved lifespan API.
The June 2026 FastAPI pull-request list includes an open
`dependency_scope="lifespan"` feature proposal (#15112), reinforcing that this
area remains active.

- [FastAPI lifespan documentation](https://fastapi.tiangolo.com/advanced/events/)
- [FastAPI roadmap #10370](https://github.com/fastapi/fastapi/issues/10370)
- [Lifespan dependency discussion #11742](https://github.com/fastapi/fastapi/discussions/11742)

**Verified QuackAPI gap.** Server start/stop is available through
`quackapi_serve` and `quackapi_stop`, but there is no declared startup/shutdown
registry or a documented ordering/failure contract.

**Recommendation.** Design `CREATE LIFECYCLE` only after request cancellation,
draining, and middleware semantics are settled. Startup/teardown SQL needs an
explicit transaction and retry contract; a partial startup must reverse only
successfully-started hooks. This is valuable but a higher-risk first feature.

## 6. SQL-native dependencies, not a Python-style container

FastAPI users repeatedly ask for dependencies usable in lifecycle and external
contexts, but FastAPI maintainers note that its resolver is request-coupled and
not yet a prioritized standalone component. QuackAPI should not copy an
automatic object container into C++. Its natural equivalent is a named,
validated SQL dependency that supplies request-bound values on the same
connection. Define that after middleware establishes lifecycle and override
semantics.

- [FastAPI external DI discussion #12131](https://github.com/fastapi/fastapi/discussions/12131)
- [Dependencies in lifespan discussion #11742](https://github.com/fastapi/fastapi/discussions/11742)

The design must include caching scope, cleanup, `quackapi_request` overrides,
and a rule that dependencies cannot expose unauthenticated claims or bypass row
access/masking policy. It is potentially compelling but too broad to start
before the smaller contracts above.
