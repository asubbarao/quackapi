# FastAPI transport and production demand

This is a primary-source demand scan for the features that most often shape a
FastAPI adoption decision. It is a product planning input, not a popularity
survey: the signals below come from FastAPI's own documentation, repository
issues/discussions, and current pull requests. The benchmark should measure
each transport with matched semantics and resource budgets before making a
performance claim.

## Priority order

| Priority | Demand signal | Why users care | QuackAPI fit | Suggested proof |
|---|---|---|---|---|
| P0 | Streaming HTTP and SSE | AI token streams, notifications, logs, and resumable browser feeds are standard API workloads. FastAPI's SSE implementation includes keep-alive pings, no-cache, proxy-buffering protection, typed events, and `Last-Event-ID` resumption. | QuackAPI already has a `CREATE STREAM` surface and an SSE regression test. Make typed event fields, disconnect cancellation, bounded buffering, and resume IDs easy to declare. | A matched POST/GET stream benchmark: time to first event, inter-event jitter, completed streams, cancellation, reconnect/resume, and bytes delivered. |
| P0 | Authentication and authorization | FastAPI makes OAuth2, bearer, API-key, cookie, and OpenID Connect schemes appear in OpenAPI. Users expect external identity providers, tenant claims, route-level policy, and generated client compatibility. | QuackAPI has API-key/JWT primitives and route auth hooks. Add explicit OpenAPI security scheme generation, JWKS/OIDC discovery with bounded cache/refresh, and policy examples that cover views and GraphQL. | Conformance cases for bearer/API-key/OIDC claims, expired keys, tenant isolation, and OpenAPI output; include denial latency and cache refresh behavior. |
| P1 | WebSockets | Bidirectional sessions remain a common FastAPI differentiator for chat, dashboards, and device control. FastAPI documents dependency use for WebSockets, while repository questions repeatedly cover authentication and high stream counts. | Requires a long-lived connection lifecycle, bounded per-connection memory, ping/pong, cancellation, admission limits, and auth during handshake. Treat this as a separate transport layer rather than stretching request routes. | Echo plus broadcast benchmark with authenticated handshake, 1/100/1k connections, slow consumers, reconnects, and graceful shutdown. |
| P1 | JSONL and general streaming responses | FastAPI's `StreamingResponse` covers arbitrary async/normal generators and the framework warns that cancellation needs an await point. This is useful for exports, query results, and model output even when SSE framing is not wanted. | Map a query cursor or `CREATE STREAM` to chunked JSONL with explicit flush/backpressure and client disconnect propagation. | Compare first-byte latency, sustained throughput, peak RSS, cancellation cleanup, and output correctness for small and large rows. |
| P1 | Production lifecycle and resource controls | FastAPI's deployment guidance separates TLS, startup, restarts, replication, and per-process memory. The docs explicitly describe Uvicorn workers versus one process per container and warn that each process has its own memory. | Provide one production recipe with TLS termination, health/readiness, graceful drain, worker budgets, request/body/header limits, query deadlines, rate limits, structured logs, and metrics. Keep worker and pool budgets explicit. | Kill/restart and rolling-drain tests; overload tests with 429/503 behavior; verify deadlines, memory bounds, and no request loss during shutdown. |
| P2 | Sessions and browser-oriented auth | A long-running FastAPI issue asks for first-class session support because Starlette's signed-cookie middleware does not integrate cleanly with generated OpenAPI security schemas. | Offer a documented signed session or external session-store pattern only if it can be represented in OpenAPI and has clear key rotation/CSRF behavior. | Login/logout/expiry/rotation/CSRF tests with multiple workers and restart. |

## What the sources say

FastAPI's security guide lists API keys, HTTP bearer/basic, OAuth2 flows, and
OpenID Connect as OpenAPI security schemes, and explains that the framework's
helpers integrate those schemes into interactive documentation:

- [FastAPI Security](https://fastapi.tiangolo.com/tutorial/security/)
- [Security first steps](https://fastapi.tiangolo.com/tutorial/security/first-steps/)
- [External OAuth providers discussion #9137](https://github.com/fastapi/fastapi/discussions/9137)
- [First-class session support issue #754](https://github.com/fastapi/fastapi/issues/754)

FastAPI now has a first-class SSE guide. It documents `EventSourceResponse`,
typed events, `Last-Event-ID`, POST streaming, keep-alive pings, cache control,
and `X-Accel-Buffering: no`:

- [Server-Sent Events](https://fastapi.tiangolo.com/tutorial/server-sent-events/)
- [SSE reference](https://fastapi.tiangolo.com/reference/sse/)
- [StreamingResponse and custom responses](https://fastapi.tiangolo.com/advanced/custom-response/)
- [SSE content schema issue #15401](https://github.com/fastapi/fastapi/issues/15401)
- [SSE schema discussion #15767](https://github.com/fastapi/fastapi/discussions/15767)

WebSocket authentication and high-concurrency streaming are recurring repository
questions rather than a single missing decorator:

- [WebSocket auth issue #2300](https://github.com/fastapi/fastapi/issues/2300)
- [WebSocket stream concurrency discussion #7118](https://github.com/fastapi/fastapi/discussions/7118)
- [FastAPI WebSockets guide](https://fastapi.tiangolo.com/advanced/websockets/)

The deployment guide says to reason separately about HTTPS, startup, restarts,
worker replication, and memory. It recommends a single process per container
when the orchestrator owns replication, and describes process-per-worker memory
costs. That is directly relevant to a fair QuackAPI comparison:

- [Deployment concepts](https://fastapi.tiangolo.com/deployment/concepts/)
- [HTTPS termination](https://fastapi.tiangolo.com/deployment/https/)
- [FastAPI in containers](https://fastapi.tiangolo.com/deployment/docker/)
- [Server workers](https://fastapi.tiangolo.com/deployment/server-workers/)
- [Worker scaling discussion #7351](https://github.com/fastapi/fastapi/discussions/7351)

Finally, performance discussions repeatedly point out that sync `def` handlers
use a threadpool, async handlers and worker count change the result, and
resource-constrained deployments cannot treat `--workers` as a free speed
switch:

- [Performance discussion #7320](https://github.com/fastapi/fastapi/discussions/7320)
- [Unexpected concurrency discussion #4358](https://github.com/fastapi/fastapi/discussions/4358)
- [Production memory/workers discussion #9145](https://github.com/fastapi/fastapi/discussions/9145)

## Roadmap fit

The most compelling QuackAPI path is a typed streaming surface backed by
DuckDB query execution: declare an SQL stream, get JSONL or SSE framing, and
retain cancellation, bounded buffering, auth, and observability at the server
boundary. The existing stream and queue primitives reduce the amount of new
runtime machinery needed for this path.

GraphQL and route syntax improvements should follow the transport contract. A
GraphQL subscription should use the same stream lifecycle and policy checks as
an ordinary `CREATE STREAM`; it should not become a separate unauthenticated
escape hatch. Likewise, generated OpenAPI must describe the auth and stream
media types users rely on when selecting FastAPI.

The benchmark work in `bench/` now records per-cell validity, preserves raw
runs, scopes write durability checks, and uses equal aggregate worker/pool
budgets. Add the transport cases above only after those gates are green, and
publish throughput alongside successful request rate, tail latency, first-byte
or first-event latency, cancellation outcomes, memory, and delivery guarantees.
