# quackapi

**A web framework inside DuckDB.**

Routing, typed parameter validation (path, query, body, header, cookie), constraint checking, 422 error shapes, JSON serialization, auto-generated OpenAPI 3.0, Swagger UI, form bodies, multipart uploads, redirects, per-route response headers (including Set-Cookie), CORS, and background/cron work — all as SQL and data. The same registry and semantics are available two ways:

- Pure SQL reference tier (`framework.sql`): `handle_request` is a table macro. The router *is* a query.
- Compiled C++ extension (`ext-cpp/`): routing, validation, and static responses run in native code; DuckDB executes only the rendered handler for dynamic routes.

The extension embeds a raw-socket pthread HTTP server (16-worker accept loop) directly in the DuckDB process: `serve_brain(port, db_path)` + `block_forever()`. Routes are rows in a `routes` table (or `CREATE ROUTE` / `DROP ROUTE` via ParserExtension). The pure-SQL implementation is the executable specification and oracle; the extension is byte-compared against it.

quackapi is the *framework* layer (routing, validation, serialization, OpenAPI, route DDL). It is designed to compose with listeners and meshes (httpserver, airport, quack, quackscale) rather than replace them. Typical deployment: one DuckDB holds your data; a serving DuckDB loads quackapi and your route handlers (ATTACH the data side or keep them separate). Use quackscale for Tailscale mesh / private-network serving without public ports, and quack_oauth for OAuth 2.1/OIDC — do not reimplement them here.

## Hero

```sql
CREATE TABLE users (id INTEGER, name VARCHAR, age INTEGER);
INSERT INTO users VALUES (1, 'alice', 30);

CREATE ROUTE get_user GET '/users/{id}' (id INT) AS
  SELECT to_json(u) AS body FROM users u WHERE u.id = {id};

CREATE ROUTE create_user POST '/users' (name TEXT, age INT) STATUS 201 AS
  INSERT INTO users (name, age) VALUES ({name}, {age})
  RETURNING to_json(users) AS body;

CREATE ROUTE search GET '/search' (q TEXT, limit INT <= 100 ?) AS
  SELECT coalesce(json_group_array(to_json(u)), '[]') AS body
  FROM (SELECT * FROM users
        WHERE starts_with(lower(name), lower({q}))
        ORDER BY id LIMIT coalesce({limit}, 100)) u;
```

```bash
curl http://127.0.0.1:18099/users/1
# {"id":1,"name":"alice","age":30}

curl http://127.0.0.1:18099/users/abc
# 422 {"detail":[{"type":"int_parsing","loc":["path","id"],"msg":"Input should be a valid integer, unable to parse string as an integer","input":"abc"}]}

curl -X POST -H 'content-type: application/json' \
  -d '{"name":"zoe","age":31}' http://127.0.0.1:18099/users
# 201 {"id":...,"name":"zoe","age":31}

curl 'http://127.0.0.1:18099/search?q=al&limit=2'
# [{"id":1,"name":"alice","age":30}, ...]

open http://127.0.0.1:18099/docs
# Swagger UI, "Try it out" works against the live schema
```

Tier-1 (no server process at all):

```bash
( cat framework.sql app.sql; \
  echo "SELECT * FROM handle_request('GET','/users/1','{}','');" ) \
| duckdb :memory:
```

## Install & quickstart

**Aspiration (not yet published):**

```sql
INSTALL quackapi FROM community;
LOAD quackapi;
```

**Today (build from source):**

```bash
git clone https://github.com/aloksubbarao/quackapi
cd quackapi/ext-cpp
make release
# artifact: build/release/extension/quackapi/quackapi.duckdb_extension
```

Boot the server (compiled tier):

```bash
duckdb -unsigned quackapi.db <<'EOF'
LOAD 'build/release/extension/quackapi/quackapi.duckdb_extension';
.read ../framework.sql
.read ../app.sql
SELECT serve_brain(18099, 'quackapi.db');
SELECT block_forever(0);
EOF
```

Override port/DB via the calls or edit `launch_server.sql` + `./run.sh`.

Pure reference (Tier-1) needs no extension and no background process.

## FastAPI side-by-side

| Concept                  | FastAPI (Python)                                      | quackapi (SQL / data)                                      |
|--------------------------|-------------------------------------------------------|------------------------------------------------------------|
| Endpoint registration    | `@app.get("/users/{id}")` decorator + function        | `CREATE ROUTE ...` or `INSERT INTO routes SELECT * FROM register_route(...)` |
| Path param + type        | `id: int = Path(...)`                                 | `(id INT)` in route declaration; structural segment match  |
| Query param + constraint | `limit: int = Query(..., le=100)`                     | `(limit INT <= 100 ?)` (optional via `?`)                  |
| Body model               | `class Create(BaseModel): name: str; age: int`        | `(name TEXT, age INT)` on POST; same for form bodies       |
| Validation error         | 422 `{"detail":[{"type":..., "loc":[...], "msg":..., "input":...}]}` | Identical shape (type/loc/msg/input) via TRY_CAST + aggregation |
| OpenAPI                  | Pydantic type-hint walk at import time                | `SELECT` over `routes` + `param_schema` (generated per request for /openapi.json) |
| Docs                     | Swagger UI mounted automatically                      | `/docs` route whose body is the Swagger HTML (static)      |
| Background work          | `BackgroundTasks` + dependency injection              | `dispatch_async(...)` (detached pthread + internal execution channel) or `cronjob` extension |
| Extra features (search, postgres, templates, etc.) | pip + glue + async client                             | `LOAD <ext>` + one route registration (zero framework changes) |

The 422 contract, constraint messages ("less than or equal to 100"), and OpenAPI 3.0 structure were validated against a FastAPI mirror.

## Architecture (two tracks, one interface)

Everything funnels through `handle_request(method, path, headers, body)` → `(status_code, content_type, body, handler_sql, resp_headers)`.

**Pure reference tier** (`framework.sql`):
- `routes` + `param_schema` are the registry (config-as-data, the analog of decorator tables).
- Routing: split both pattern and request path into segment arrays; position-by-position match + most-literal tie-break (no regex).
- Validation: `TRY_CAST` + constraint checks (`le`/`ge`/`required`); all failures aggregated into FastAPI-shaped `detail[]`.
- OpenAPI and `/docs` are queries (or pre-rendered statics) over the registry.
- Handler SQL is templated with literal values and executed by the caller.
- This tier is the oracle. ~1k req/s floor on the pure-SQL router (13-operator OLAP query per request).

**Compiled extension tier** (`ext-cpp/`):
- At `serve_brain` boot, the C++ layer loads `routes`/`param_schema`/`route_headers` into in-process structs (read-only after).
- Per request: segment match, param extraction, validation, and `{param}` → SQL literal templating happen in C++.
- Static routes, 404, 422, `/openapi.json`, `/docs` are served with zero DuckDB calls.
- Dynamic routes: only the final rendered handler SQL reaches DuckDB.
- Same `routes` table and `CREATE ROUTE` surface; same 422 and OpenAPI semantics; byte-identical outputs on the oracle harness (one documented non-determinism in JSON key order for OpenAPI paths).
- Static ~39–44k req/s; dynamic routed+validated ~26–35k req/s (Apple Silicon, ab, keep-alive, 0 failed requests).

The C server is a 16-pthread accept loop. Workers parse headers/cookies (including form and multipart), call the route decision, execute or serve, and write the response. `block_forever()` keeps the process resident.

Writes that must run outside the request's connection use the internal execution channel (detached pthread posting to a loopback or fresh connection). This is what buys MVCC/OCC for concurrent writes.

CORS, middleware, and pure-tier DI live in the SQL layer today (oracle path); the compiled path focuses on the hot routing+validation surface.

## Verification

All claims are backed by re-runnable artifacts in the repo.

- 56 sqllogictest assertions green (`make test` in ext-cpp/; covers DDL, routing decisions, error paths).
- 44-case oracle-parity harness: C++ `quack_route_decision` compared to `SELECT * FROM handle_request(...)` across path/query/body/header/cookie/form, all 422 variants, statics, openapi, redirects, 404/405; 100% pass (byte-identical except documented OpenAPI key order).
- 100/100 fuzz oracle: property tests (adversarial paths, missing/overflow/constraint cases) against the pure SQL brain.
- 87-case FastAPI conformance suite on the reference tier (62 exact matches; remaining are documented intentional deviations for RFC correctness, stricter coercion, trailing-slash policy, HEAD semantics, etc.). See `test/conformance/`.
- Live HTTP Tier-2 curls and ApacheBench runs with exact PID-isolated lifecycle.
- Composability receipts regenerated via `test/compose_receipts.sh` and `test/compose_cron_fire.sh`.

Parity is the gate: any change to routing/validation must keep the two tracks identical on the matrix.

## Composability

Handlers are SQL. Any stock community extension composes inside a request with zero changes to the framework.

From `COMPOSABILITY.md` (regenerable receipts):

- `postgres` — ATTACH a live Postgres and serve REST over its tables (PostgREST claim) with the same validation pipeline.
- `cronjob` — schedule background jobs; firing proof (executions continue after the scheduling statement returns).
- `fts`, `rapidfuzz`, `markdown`, `crypto` (HMAC), `tera` (templates), `curl_httpfs` (parallel fan-out), `json_schema`, `bitfilters`, etc.
- Full-text search, fuzzy lookup, server-rendered HTML, webhook signing, HTTP fan-out — all one `LOAD` + route registration.

FastAPI's equivalent surface (Elasticsearch client, Celery, Redis queue, Jinja, httpx gather, Pydantic schema lib, ...) requires a venv of dependencies per feature. Here the weight is the extension itself.

Measured footprint for the receipt set: ~155–170 MB total. A typical production FastAPI venv starts at 300 MB–1 GB before application code.

Validation still applies: a missing required field on a composed route yields the exact 422 shape.

## Benchmarks

Apple Silicon (Mac16,5, 16 logical cores), ApacheBench (`ab -n 8000 -c N -k`), fresh DB/port, zero failed requests. Numbers from the compiled tier (B2 result + later non-reg runs).

| Route                  | Exercises                     | c=8     | c=64    | vs pure-SQL floor |
|------------------------|-------------------------------|---------|---------|-------------------|
| `/health`              | static, zero DB               | ~38–40k | ~41–44k | ~29–40×             |
| `/users`               | list → 1 handler query        | ~25–27k | ~31–32k | ~23–30×             |
| `/users/1`             | path param → point query      | ~26k    | ~28–35k | ~19–28×             |
| `/search?q=al&limit=2` | query params + filtered query | ~14–15k | ~19k    | ~11–18×             |

Pure reference tier floors at ~1–2.3k req/s (the cost of a 13-operator analytical query per request, including macro bind/optimize). The compiled tier removes the router from SQL while preserving the exact same registry, validation rules, and outputs.

The transport layer itself (`/ping` pure-C fast path) does ~38k req/s. The gap to 34k+ on trivial point queries (`/q2` control) is the remaining per-operator tax in the handler path.

## Edges & limitations

Honest limits are part of the contract. See `edges.md` for the full hypothesis → probe → verdict ledger (every entry backed by a re-runnable experiment).

Short version:

- **Single-writer semantics (REAL, bounded)**: writes serialize at DuckDB's writer. Dispatch + OCC + retry recovers conflicts (16/16 with retry vs 6/16 without in the probe). Throughput is bounded by the single writer, not by the HTTP layer.
- **No WebSockets yet (spec'd)**: RFC 6455 transport and frame codec exist and were verified in C (SHA-1 + base64 handshake, mask/unmask). Full integration (kind='ws', route DDL, per-message handler SQL) is in `docs/specs/WS_SPEC.md` and `serve_ws.sql` but not wired into the main `serve_brain` path.
- **No TLS**: the listener binds localhost by default. Front it with a reverse proxy or run inside a tailnet (quackscale). sockaddr layout is currently macOS/BSD-specific.
- **C layer can crash the process**: no sandbox. Defensive C + process supervision is the mitigation.
- **Query values not percent-decoded**: `?q=a%40b` reaches the handler as `a%40b`. Documented conformance gap.
- **Multipart**: small bodies (< ~64 KB single read) work; larger uploads hit the fixed buffer / ducktinycc allocation limits (PARTIAL).
- **DI teardown**: setup + inject is real; guaranteed `finally` across dispatches is not (PARTIAL).
- **Open transaction across request**: one-shot dispatch model has no yield-style shared txn (REAL).

None of these are hidden.

## Roadmap

- WebSocket server integration (kind + DDL + radio composition)
- TLS listener
- Windows portability (sockaddr, build matrix)
- Hot-path simplification to close more of the gap to the 34k trivial-query floor on dynamic routes
- Community catalog publication (`INSTALL quackapi FROM community`)
- Additional first-class DDL (`CREATE POLICY`, rate limiting, etc.) modeled on the `CREATE ROUTE` precedent

## License

MIT

---

This is not "DuckDB plus a web framework." It is the framework re-derived so that routing, validation, and OpenAPI are ordinary SQL data and queries — or the same surface compiled to native speed inside the same process. The map of where the abstraction holds and where it tears is in the repo.