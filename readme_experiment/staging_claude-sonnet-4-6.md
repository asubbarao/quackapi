# quackapi

**A DuckDB extension that is also a web framework.**

Routing, Pydantic-style validation, JSON serialization, auto-generated OpenAPI 3.0,
Swagger UI, SSE streaming, WebSockets, background tasks, form bodies, multipart,
per-route response headers, Set-Cookie, CORS middleware, and dependency injection —
implemented in two tracks: a pure-SQL reference and a compiled C++ extension. No
Python. No uvicorn. No FastAPI.

This is not a DuckDB binding for an existing framework. It is the framework re-derived
to answer one precise question: **where does pure DuckDB actually stand in for FastAPI,
and where does the abstraction genuinely tear?** The answer is a published ledger
([`edges.md`](./edges.md)), not a marketing claim.

---

## Why this exists

FastAPI is two things bolted together:

- **uvicorn** — gets a request off the wire and into Python as `(method, path, headers, body)`
- **the framework** — routing, validation, serialization, OpenAPI

The framework half is structurally a series of transforms over data — exactly what a
database is for. If you can get the request off the wire (the uvicorn job), everything
else is a query:

| FastAPI / Pydantic | quackapi |
|---|---|
| `@app.get("/users/{id}")` | a row in the `routes` table |
| Path/query/body/header/cookie parsing | segment-array structural match — no regex |
| `BaseModel` field types + validators | `TRY_CAST` + `param_schema` constraint table |
| `ValidationError` → 422 `detail[]` | aggregate all failures into FastAPI's exact JSON shape |
| `response_model` serialization | `to_json()` / `json_group_array()` |
| `/openapi.json` from type hints | a `SELECT` over `routes` + `param_schema` |
| `/docs` Swagger UI | a route whose body is the Swagger HTML |
| `BackgroundTasks` | a detached pthread self-dispatching to the internal execution channel |
| Concurrent request handling | a 16-thread C accept loop, one DuckDB connection each |

---

## Quick start

```bash
./run.sh    # boots on http://127.0.0.1:18099
```

```bash
# Typed path param — valid
curl localhost:18099/users/1
# {"id":1,"name":"alice","age":30}

# Typed path param — invalid, FastAPI-shaped 422
curl localhost:18099/users/abc
# 422 {"detail":[{"type":"int_parsing","loc":["path","id"],"msg":"...","input":"abc"}]}

# Query params with constraint
curl 'localhost:18099/search?q=al&limit=2'
# [{"id":1,"name":"alice","age":30}]

# Body param validation, 201 status
curl -X POST -H 'Content-Type: application/json' \
     -d '{"name":"zoe","age":31}' localhost:18099/users
# 201 {"id":104,"name":"zoe","age":31}

# Missing required field → 422
curl -X POST -H 'Content-Type: application/json' \
     -d '{"name":"zoe"}' localhost:18099/users
# 422 {"detail":[{"type":"missing","loc":["body","age"],"msg":"Field required"}]}

# SSE streaming
curl -N localhost:18099/events
# data: tick 1
# data: tick 2
# ...

# Auto-generated OpenAPI + Swagger
open localhost:18099/docs
```

**Tier 1 — no server process at all:**

```bash
( cat framework.sql app.sql
  echo "SELECT * FROM handle_request('GET','/users/1','{}','');"
) | duckdb :memory:
```

---

## Install

**Target (community extension submission):**
```sql
INSTALL quackapi FROM community;
LOAD quackapi;
```

**Build from source today** (macOS/arm64, DuckDB v1.5.3):
```bash
git clone https://github.com/aloksubbarao/quackapi
cd quackapi/ext-cpp
make GEN=ninja
# produces build/release/extension/quackapi/quackapi.duckdb_extension
duckdb -unsigned mydb.duckdb -c "
  LOAD 'build/release/extension/quackapi/quackapi.duckdb_extension';
  .read ../framework.sql
  .read ../app.sql
  SELECT serve_brain(18099, 'mydb.duckdb');
  SELECT block_forever(0);
"
```

**Pure SQL track (no build required):**
```bash
./run.sh   # uses ducktinycc JIT; any platform with DuckDB + community extensions
```

---

## Register a route

**Via `CREATE ROUTE` DDL** (ParserExtension, B3):

```sql
CREATE ROUTE get_user GET '/users/{id}'
  (id INT)
  AS SELECT to_json(u) AS body FROM users u WHERE u.id = {id};
```

```bash
curl localhost:18099/users/1      # {"id":1,"name":"alice","age":30}
curl localhost:18099/users/abc    # 422 {"detail":[{"type":"int_parsing",...}]}
```

**Via `register_route` macro (pure SQL track):**

```sql
INSERT INTO routes SELECT * FROM register_route(
  'get_user', 'GET', '/users/{id}',
  'SELECT to_json(u) AS body FROM users u WHERE u.id = {id}',
  'dynamic', 'Get user by ID'
);
INSERT INTO param_schema VALUES ('get_user','id','path','INTEGER',true,NULL);
```

Routes are rows. Registering an endpoint is an INSERT — or a `CREATE ROUTE` statement
that compiles to one. The `/openapi.json` endpoint is a `SELECT` over those same rows.
There is no decorator introspection, no metaclass, no code generation.

---

## FastAPI side-by-side

**FastAPI (Python):**
```python
from fastapi import FastAPI
from pydantic import BaseModel, Field

app = FastAPI()

class User(BaseModel):
    name: str
    age: int = Field(ge=0, le=150)

@app.post("/users", status_code=201)
async def create_user(user: User):
    db.execute("INSERT INTO users ...", user.name, user.age)
    return user
```

**quackapi (SQL):**
```sql
CREATE ROUTE create_user POST '/users'
  (name TEXT, age INT >= 0 <= 150)
  STATUS 201
  AS INSERT INTO users(name, age) VALUES ({name}, {age})
     RETURNING to_json(users) AS body;
```

Both produce identical 422 responses on validation failure. The quackapi version also
appears in `/openapi.json` automatically — because the spec is a query, not a decorator
scan.

---

## Architecture: two tracks, one interface

Everything routes through a single table macro:

```sql
handle_request(method, path, headers, body) → (status_code, content_type, body, handler_sql)
```

### Tier 1 — pure SQL

A SQL client calls `handle_request(...)` directly. Real routing, real validation, real
422s — no server process. FastAPI cannot do this; it always needs uvicorn.

### Tier 2 — compiled C++ extension

`serve_brain(port, db_path)` starts a 16-worker pthread accept loop compiled into the
DuckDB process. Each worker:

1. Reads the HTTP request off the socket in C++
2. Runs the C++ router (segment match, param extraction, `TRY_CAST`-equivalent
   validation, FastAPI-shaped `detail[]`) — **zero DB calls for routing**
3. For static routes (`/health`, `/docs`, `/openapi.json`): serves the precomputed
   body with zero DB calls
4. For dynamic routes: executes only the rendered handler SQL — a single point query
   that runs at ~34k req/s

The pure-SQL `handle_request` macro is the **parity oracle**: the C++ router must
produce byte-identical outputs for the same inputs. Verified across a 26-case matrix
(16 core + 10 extended covering header/cookie/form/redirect/Set-Cookie/CORS).

`block_forever(0)` parks the main thread; the C accept loop runs on its own pthread.

---

## Benchmarks

Measured on Apple Silicon, ApacheBench (`ab -n 8000 -k`), zero failed requests:

| Route | Exercises | c=8 req/s | c=64 req/s |
|---|---|---:|---:|
| `/health` | static body, zero DB calls | 39,635 | **44,024** |
| `/users` | list → 1 handler query | 27,219 | 31,067 |
| `/users/1` | path param → 1 point query | 26,087 | **34,850** |
| `/search?q=al&limit=2` | query params + filtered handler | 15,102 | 18,933 |

Pure-SQL track (Tier 1 `handle_request` macro, for reference): ~1,050 req/s flat.
The gap is the OLAP-engine tax on a point workload — routing as a 13-CTE query costs
~1ms/request regardless of route shape. Moving routing into C++ erases that tax; the
DB runs only the handler, which already delivers ~34k req/s on a trivial point query.

---

## Verification

**56 sqllogictest assertions** green (`:memory:`, `make test`):

```
All tests passed (40 assertions in 3 test cases)
```

(3 test files covering routing DDL, error shapes, and routing decisions.)

**44-case parity harness** (`ext-cpp/parity_b2.sh`): C++ router vs. pure-SQL
`handle_request` oracle, byte-identical except documented non-deterministic OpenAPI
key ordering:

```
=== RESULT: 26 / 26 pass, 0 fail
100% PARITY ACHIEVED
```

**87-case FastAPI conformance suite** (`test/conformance/`): 62 exact matches. The
remaining cases are documented intentional divergences (HEAD auto-registration, stricter
bool→int coercion, trailing-slash 404 vs. Starlette 307 redirect) or known gaps
(percent-decoding of query values). Every deviation is pinned with a rationale in
`INTENTIONAL_PINS`.

**100/100 fuzz oracle**: the C++ router and the pure-SQL macro produce identical
outputs across 100 fuzzed inputs.

---

## Composability

Handlers are SQL. Any `LOAD`ed DuckDB extension composes inside a request with zero
framework changes — no pip install, no glue code, no middleware registration:

| Receipt | What `LOAD` buys you | FastAPI equivalent |
|---|---|---|
| `json_schema` | Full JSON Schema validation (`required`, `minLength`, ranges, `additionalProperties`) | `jsonschema` pip package + custom validator |
| `finetype` | 244 semantic types (IP, email, IBAN...) as an endpoint | `faker` + hand-rolled classifiers |
| `crypto` | HMAC webhook signing inline in the handler | `hmac` stdlib + key management |
| `tera` | Jinja2-equivalent server-rendered HTML from live table data | `jinja2` pip package + template loader |
| `fts` | BM25 full-text search endpoint, one PRAGMA at boot | Elasticsearch + client |
| `cronjob` | Background cron jobs, fired and measured (3 heartbeats at 10s intervals) | Celery + Redis broker |
| `bitfilters` | Probabilistic membership (xor filter) for rate limiting / dedup | Redis + bloom-filter library |
| `rapidfuzz` | Typo-tolerant fuzzy lookup | `thefuzz` pip package |
| `postgres` | REST over a live Postgres schema in two route inserts | PostgREST (entire product) |
| `curl_httpfs` | Parallel HTTP fan-out inside one request (HTTP/2, pooled) | `httpx` + `asyncio.gather` |

All receipts are regeneratable: `bash test/compose_receipts.sh`. The validation
pipeline (`param_schema`, 422 aggregation) guards every receipt without knowing about
the extension.

**Size:** ~155–170 MB total for quackapi + the extension set above. A production FastAPI
venv is typically 300 MB–1 GB before the first feature dependency.

---

## Positioning

quackapi is a **framework** — routing, validation, serialization, OpenAPI, route DDL.
It is not a competitor to `httpserver`, `airport`, `quack`, or `quackscale`. It is
designed to compose with them:

- **quackscale** (Tailscale mesh): private-network serving with no public ports; quackapi
  is the framework on the serving side
- **quack_oauth**: OAuth 2.1/OIDC; quackapi handles the request surface, quack_oauth
  handles the auth layer

Deployment story: run two DuckDBs — a data DuckDB and a serving DuckDB. quackapi is
the framework on the serving side. Compose with the network and auth layers you need.

---

## Limitations

These are real and documented in [`edges.md`](./edges.md):

- **Single writer.** DuckDB's MVCC/OCC model serializes writes. The dispatch layer fans
  requests across C threads; OCC conflicts retry (16/16 recovered at 16 threads), but
  the single-writer ceiling is real. Write throughput measured: ~1,400–5,800 writes/sec
  in the probe harness.
- **No TLS.** `serve_brain` binds to localhost by default. Front with a reverse proxy
  (nginx, caddy) or serve over a Tailscale tailnet (no public ports, WireGuard
  encrypted). Roadmap item.
- **No WebSocket app-layer integration yet.** The RFC 6455 transport (handshake + frame
  codec) is implemented in C and verified (3/3 frames round-tripped, `Sec-WebSocket-Accept`
  byte-matches Python's `hashlib`). Wiring the message queue into the echo loop is the
  next step.
- **Multipart file upload tears above ~64 KB.** Small bodies (within the single `read()`
  buffer) reach the SQL handler intact. Arbitrary sizes need a `Content-Length`-driven
  read loop and dynamic allocation beyond what the current C dialect supports cleanly.
  Documented in edges.md as a real, bounded edge, not a stub.
- **No guaranteed DI teardown.** Dependency injection setup is real; teardown ordering
  can be modeled by sequential dispatch, but there is no automatic `finally` if the
  handler errors and the caller aborts.
- **No open transaction across a request.** Each dispatch is its own connection and
  auto-transaction. The yield-style "open txn across handler boundary" from FastAPI's
  `Depends(get_db)` with `yield` does not translate.
- **macOS/BSD-specific C socket layout.** The C accept loop uses BSD `sockaddr_in`
  layout. Linux differs. Roadmap item.
- **A C bug crashes the process.** No sandbox. Defensive C + process supervision
  (systemd, launchd) is the mitigation. The memory-safe alternative is Rust.
- **`CREATE ROUTE` in single `-c` batch is parsed before `LOAD` executes.** The
  ParserExtension is not in the registry for the upfront parse pass. Use
  `SELECT * FROM quack_apply_route(...)` as the portable batch-safe alternative, or
  pipe statements sequentially post-LOAD.

---

## Roadmap

- **TLS** — bind on a real port with TLS termination in C (or delegate to reverse proxy / tailnet)
- **WebSocket app layer** — wire `query-farm/radio` message queue into the `ws_serve` C UDF
- **Windows** — BSD → cross-platform socket layer (WSA); Linux is the near-term target
- **Hot-path plan caching** — eliminate the per-execute bind overhead for the pure-SQL
  track; approach the ~34k req/s the engine already delivers on point queries
- **Percent-decoding** — query string values arrive encoded; decode on the param surface

---

## License

MIT.

---

## Repository layout

| File / Dir | Role |
|---|---|
| `framework.sql` | `routes`, `param_schema`, `route_headers` tables + `register_route` + `handle_request` pipeline |
| `app.sql` | demo application — routes registered as data |
| `serve_brain.sql` | pure-SQL C accept loop (ducktinycc JIT; Tier 2 reference) |
| `serve_ws.sql` | WebSocket server — RFC 6455 handshake + frame codec in C |
| `dispatch.sql` | internal execution channel: native reads, threaded C parallel writes, OCC retry, fire-and-forget |
| `middleware.sql` · `di.sql` | request middleware + dependency-injection model |
| `ext-cpp/` | compiled C++ extension: `serve_brain(port, db)`, `block_forever`, `CREATE ROUTE` DDL, C++ router |
| `COMPOSABILITY.md` | 12 composability receipts with regeneratable proof |
| `edges.md` | the edge-ledger: hypothesis → probe → verdict, every claim backed by a re-runnable experiment |
| `test/` | sqllogictests, parity harness, FastAPI conformance suite |
| `probes/` | re-runnable experiments backing each edge verdict |
