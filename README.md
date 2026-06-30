# quackapi

**A FastAPI-equivalent web framework written in pure DuckDB SQL.**

Routing, Pydantic-style validation, JSON serialization, auto-generated OpenAPI +
Swagger UI, Server-Sent Events, WebSockets, background tasks, middleware, and true
concurrent writes — built from primitives on stock, publicly-installable DuckDB
extensions. No application code in any other language. The HTTP server itself is C,
JIT-compiled *inside* the DuckDB process at runtime.

This is not a DuckDB binding for an existing framework. It is the framework,
re-derived from scratch to answer one question precisely: **where can pure DuckDB +
self-dispatch actually stand in for Python/FastAPI, and where does the abstraction
genuinely tear?** That map is [`edges.md`](./edges.md) — the point of the project.

---

## The thesis

FastAPI is two things bolted together: **uvicorn** (gets a request off the wire and
into Python as `(method, path, headers, body)`) and **the framework** (routing,
validation, serialization, OpenAPI). The framework half is, structurally, a series of
transforms over data — exactly what a database is for:

| FastAPI / Pydantic concept | quackapi implementation |
|---|---|
| `@app.get("/users/{id}")` decorator | a **row** in a `routes` table (`register_route(...)`) |
| Path/query/body parsing | segment-array structural match — **no regex** |
| `BaseModel` field types + validators | `TRY_CAST` + a `param_schema` constraint table |
| `ValidationError` → 422 `detail[]` | aggregate every failure into FastAPI's exact JSON shape |
| `response_model` serialization | `to_json()` / `json_group_array()` |
| `/openapi.json` from type hints | a **`SELECT`** over `routes` + `param_schema` |
| `/docs` Swagger UI | a route whose body is the Swagger HTML |
| `BackgroundTasks` | a detached C thread self-dispatching to a loopback |
| concurrent request handling | a 16-thread C accept-loop, one DuckDB connection each |

The honest answer to "explain Pydantic" stops being a recitation and becomes *"I
reimplemented it as `TRY_CAST` + a constraint table + error aggregation — here's what
that taught me about what it actually does."*

## Two front doors, one SQL brain

Everything routes through a single table macro,
`handle_request(method, path, headers, body) -> (status_code, content_type, body, handler_sql)`.

- **Tier 1 — zero non-DuckDB code.** A SQL client calls `handle_request(...)`
  directly. A real, working HTTP API surface with no server process at all. FastAPI
  cannot claim this — it always needs uvicorn.
- **Tier 2 — browser-native.** `serve_brain.sql` JIT-compiles a C TCP server *inside
  DuckDB* (via the `ducktinycc` extension): a 16-worker accept loop that reads the
  request, calls the *same* `handle_request`, and executes the SQL it returns. This is
  the uvicorn-equivalent, and it holds zero framework logic.

The only boundary that is genuinely non-SQL is copying "path on the wire" → "path as
an argument." That boundary is edge #1 — and it's **defeated** by compiling `accept()`
in C inside the process.

## Quick start

```bash
./run.sh                 # boots on http://127.0.0.1:18099  (DUCKDB=/path/to/duckdb ./run.sh to override)
```

```bash
curl localhost:18099/users/1                 # {"id":1,"name":"alice","age":30}
curl localhost:18099/users/abc               # 422  {"detail":[{"type":"int_parsing",...}]}
curl localhost:18099/search?q=al&limit=2     # JSON list, limit validated (max 100)
curl -X POST -d '{"name":"zoe","age":31}' localhost:18099/users   # 201  {"id":...,"name":"zoe","age":31}
curl -N localhost:18099/events               # SSE: chunked  data: tick 1 … tick 5
open localhost:18099/docs                     # Swagger UI — "Try it out" works
```

Tier 1 (no server) — load the framework and call the brain directly:

```bash
( cat framework.sql app.sql; echo "SELECT * FROM handle_request('GET','/users/1','{}','');" ) | duckdb :memory:
```

## The pillars

- **Routing** — match `(method, path)` against `/users/{id}`-style patterns by splitting
  both into segment arrays and comparing position-by-position; capture `{param}` slots;
  tie-break by most-literal-segments. No regex anywhere.
- **Validation** — join request params to `param_schema`, `TRY_CAST` to the declared
  type, check `min`/`max`/`required`/`enum`, and **aggregate all failures** into
  `{"detail":[{"loc":[...],"msg":...,"type":...}]}` — byte-for-byte FastAPI.
- **OpenAPI + Swagger** — `/openapi.json` is a `SELECT` over `routes` + `param_schema`
  (generation is a query, not type-hint introspection — strictly easier than FastAPI).
  `/docs` serves Swagger UI pointed at it.
- **Self-dispatch / concurrency** — see below.

## Self-dispatch — the concurrency engine

A pure-SQL macro cannot `EXECUTE` a runtime-built string, and `json_serialize_sql`
refuses to serialize anything but a `SELECT`. So the engine splits by statement kind
([`dispatch.sql`](./dispatch.sql), [`docs/03-self-dispatch.md`](./docs/03-self-dispatch.md)):

- **dynamic reads** run natively in-process via
  `json_execute_serialized_sql(json_serialize_sql(sql))` — no loopback.
- **dynamic writes** self-dispatch to a separate DuckDB connection over a loopback,
  which buys MVCC + OCC concurrency for free.
- **concurrent writes** fan out over N OS threads from a `ducktinycc` C client
  (`http_post` over a list does *not* parallelize); ~7.9× at 16 threads, all committed.
- **write–write conflicts** retry inside the C worker; 16/16 recover where 12/16 would
  otherwise be lost.

Every mechanism here was adopted only after a simpler/native alternative was **run and
measured failing** — the evidence trail is in the doc.

## The edge-ledger

[`edges.md`](./edges.md) is the senior signal: each entry is **hypothesis → probe →
verdict**, every verdict backed by a re-runnable experiment.

| # | Edge | Verdict |
|---|------|---------|
| 1 | Path-on-wire boundary (the uvicorn line) | **DEFEATED** — C `accept()` compiled inside DuckDB |
| 1b | Single-thread / one-request-per-statement | **DEFEATED** — pthread pool, 10 concurrent in 0.33s |
| 1c | In-process routing × SQL × thread concurrency | **REAL** trilemma — pick 2; resolution shipped |
| 2 | SSE / streaming responses | **DEFEATED** — chunked `data:` write-loop in the responder |
| 3 | WebSockets | **DEFEATED** (transport) — RFC 6455 handshake + frames in C |
| 4 | Background tasks | **DEFEATED** — detached C thread self-dispatches |
| 5 | Open transaction across a request | **REAL** — one-shot dispatch has no shared txn |
| 6 | Dependency injection w/ setup+teardown | **PARTIAL** — no guaranteed `finally` |
| 7 | Multipart file upload streaming | **PARTIAL** — tears at the 64 KB single-read ceiling |
| 8 | High write throughput / true async | **REAL (bounded)** — single writer; numbers included |

Showing exactly where it tears is the depth signal — not "X replaces Y," but a precise
map of where the abstraction holds and where it does not.

## Repo layout

| File | Role |
|---|---|
| `framework.sql` | `routes`/`param_schema` tables + `register_route` + the `handle_request` pipeline |
| `app.sql` | the demo application — routes registered **as data** (users, search, SSE, openapi, docs) |
| `serve_brain.sql` | the C HTTP server: 16-worker accept loop, header/cookie parsing, SSE streaming |
| `serve_ws.sql` | the C WebSocket server (handshake + frame codec) |
| `dispatch.sql` | self-dispatch engine: native reads, threaded-C parallel writes, OCC retry, fire-and-forget |
| `middleware.sql` · `di.sql` | request middleware (auth, logging, header injection) + dependency-injection model |
| `launch_server.sql` · `run.sh` | boot the unified server |
| `edges.md` | the edge-ledger |
| `probes/` | re-runnable experiments backing the edge verdicts |
| `test/` | Tier-1 (`handle_request` assertions) + Tier-2 (curl) test suites |

## Built on (stock `INSTALL ... ; LOAD ...` only)

`ducktinycc` (in-process C compilation), `harbor` (loopback HTTP-SQL for self-dispatch),
`curl_httpfs` (the soldered HTTP client for handlers), `shellfs`, `httpfs_timeout_retry`.
No custom-compiled C++ community extension — the whole point is that anyone can `LOAD`
and run this.

## Caveats (these are real)

A C bug crashes the process (no sandbox — defensive C + supervision is the mitigation;
Rust is the memory-safe alternative). The C socket layout is macOS/BSD-specific. The
single DB writer bounds write throughput (edge #8). DI teardown has no guaranteed
`finally` (edge #6). Multipart tears above ~64 KB (edge #7). None of these are hidden —
they are the ledger.
