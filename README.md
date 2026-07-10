# quackapi

**A FastAPI-class web framework that lives inside DuckDB.**

Routes, validation, auth, sessions, CORS, lifecycle hooks, health probes, and
event subscriptions are all rows in tables, declared with SQL DDL. Your handler
is SQL over the same database it serves. There is no ORM because there is
nothing to map — **the data layer is the framework.**

```sql
CREATE ROUTE get_user GET '/users/{id}' (id INT) AS
  SELECT to_json(u) AS body FROM users u WHERE u.id = {id};

CREATE SESSION STORE web SECRET 'change-me' COOKIE_SECURE true;
CREATE AUTH site AS SESSION ( STORE 'web' );

CREATE SUBSCRIPTION alerts ON 'redis-tcp://localhost:6379?channel=alerts'
  AS 'INSERT INTO alert_log SELECT now(), message, channel FROM msg';

SELECT serve_brain(8000, 'app.db');
```

```
$ curl localhost:8000/users/1
{"id":1,"name":"alice","age":30}
$ curl localhost:8000/users/abc
{"detail":[{"type":"int_parsing","loc":["path","id"], ...}]}   # 422, FastAPI-exact
$ open localhost:8000/docs                                     # Swagger UI, generated
```

## Why this exists

FastAPI is two things bolted together: **uvicorn** (gets `(method, path,
headers, body)` off the wire) and **the framework** — routing, validation,
serialization, OpenAPI. That second half is, structurally, a series of
transforms over data. Transforms over data are what a database does natively:

| FastAPI / Pydantic concept | quackapi implementation |
|---|---|
| `@app.get("/users/{id}")` decorator | a **row** in the `routes` table (`CREATE ROUTE` sugar) |
| Path/query/body parsing | segment-array structural match — no regex |
| `BaseModel` types + validators | `TRY_CAST` + a `param_schema` constraint table |
| `ValidationError` → 422 `detail[]` | every failure aggregated into FastAPI's exact JSON shape |
| `response_model` include/exclude | `FIELDS(INCLUDE …)` clause — column projection |
| `/openapi.json` from type hints | a **`SELECT`** over `routes` + `param_schema` |
| Session middleware + CSRF | the session store **is a table**; signed cookies, synchronizer tokens |
| `lifespan` startup/shutdown | `CREATE LIFECYCLE ON STARTUP\|SHUTDOWN AS '<sql>'` |
| background event consumers | `CREATE SUBSCRIPTION` — SQL handler per message |

## The receipts

The sharpest proof of the thesis: **FastAPI's most-upvoted feature requests of
all time — which FastAPI closed unresolved — are things quackapi ships as a few
lines of DDL.** Full scoreboard with links, mechanisms, and verification:
[`docs/FASTAPI_MOST_WANTED.md`](docs/FASTAPI_MOST_WANTED.md).

| 👍 | FastAPI asked for | outcome there | here |
|----|---|---|---|
| 83 | Pydantic ↔ SQLAlchemy bridge ([#214](https://github.com/fastapi/fastapi/issues/214)) | closed unresolved | **dissolved** — no ORM layer exists to bridge |
| 75 | first-class sessions ([#754](https://github.com/fastapi/fastapi/issues/754)) | closed unresolved | **shipped** — `CREATE SESSION STORE` |
| 65 | richer startup/shutdown events ([#617](https://github.com/fastapi/fastapi/issues/617)) | closed | **shipped** — `CREATE LIFECYCLE` |
| 62 | built-in health/readiness probes ([#1907](https://github.com/fastapi/fastapi/issues/1907)) | closed | **shipped** — `/livez` `/readyz` `/metrics` |
| 50 | response include/exclude fields ([#1357](https://github.com/fastapi/fastapi/issues/1357)) | closed | **shipped** — `FIELDS(...)` |
| 49 | auto-HEAD for GET routes ([#1773](https://github.com/fastapi/fastapi/issues/1773)) | closed | **shipped** |
| 35 | HTTPBearer 403-instead-of-401 ([#10177](https://github.com/fastapi/fastapi/issues/10177)) | closed | **correct** — 401/403 discipline verified |

## Performance

Measured head-to-head with ApacheBench against FastAPI + uvicorn (uvloop,
httptools) on the same machine, byte-identical response bodies, raw `ab` output
preserved — [`bench/BENCH_HEADTOHEAD.md`](bench/BENCH_HEADTOHEAD.md):

- quackapi dynamic `/health`: **42.6k req/s** (c8), zero failures
- FastAPI's best cell in the same matrix: 21.8k req/s — 16 workers, in-memory
  dict, no database at all
- quackapi `/search` querying a real database: 2.2× that FastAPI ceiling
- keep-alive static path: **108k req/s** (c64)

The server is a 16-worker thread pool with an instance-pool read path (one
writer, per-worker read replicas rebuilt on write) — reads scale without giving
up read-your-writes.

## Architecture: one brain, two tracks

Everything routes through a single table macro:
`handle_request(method, path, headers, body) → (status, content_type, body, handler_sql)`.

- **The oracle** — [`framework.sql`](framework.sql). The entire framework as
  pure SQL: an executable specification. You can run a real API surface with no
  server process at all: load it and call `handle_request(...)` from any DuckDB
  client. The tier-1 suite (197 checks) asserts against this directly.
- **The compiled extension** — [`ext-cpp`](https://github.com/asubbarao/quackapi-ext-cpp)
  (submodule). A DuckDB extension providing `serve_brain(port, db)` — the
  uvicorn-equivalent: accept loop, worker pool, keep-alive, gzip, graceful
  drain, SSE. It holds **no framework logic of its own**: verification,
  composition, and policy all execute the oracle's SQL. Where both tracks
  implement a surface, parity tests pin them byte-identical.

The DDL sugar (`CREATE ROUTE / AUTH / POLICY / SESSION STORE / LIFECYCLE /
SUBSCRIPTION / CORS / HEALTH CHECK`) is a parser extension; every statement is
also expressible as plain SQL against the registry tables.

## Security

The audit ledger — trust model, enforced invariants, what was found and fixed,
and what remains open — is [`docs/SECURITY.md`](docs/SECURITY.md). The rules in
one breath: untrusted input (HTTP client *and* event bus) reaches SQL only
through prepared binds; every secret comparison is constant-time through one
choke point; one verification implementation per credential type (the C server
calls the oracle's macros — there is no second copy to drift); sessions are
server-minted signed cookies with CSRF synchronizer tokens; missing credential
is 401, failed policy is 403.

## Built on the DuckDB ecosystem

quackapi is a framework layer, not a rival server — it fills the gap *above*
the excellent transport-level extensions:

- [`crypto`](https://duckdb.org/community_extensions/extensions/crypto) — HMAC
  for sessions and JWT; quackapi implements no crypto primitive of its own
- [`radio`](https://duckdb.org/community_extensions/extensions/radio) — the
  `CREATE SUBSCRIPTION` transport (WebSocket + Redis pub/sub receive threads)
- [`shellfs`](https://duckdb.org/community_extensions/extensions/shellfs),
  `json` — handler-side plumbing
- [`tributary`](https://duckdb.org/community_extensions/extensions/tributary) —
  Kafka: batch topic scans work in route handlers today; subscription support
  lands when tributary ships continuous consumption

## Quick start (from source)

Pre-v0.1 — not yet published as a community extension, so build the extension
locally (vendored DuckDB, ~one coffee first build):

```bash
git clone --recurse-submodules https://github.com/asubbarao/quackapi
cd quackapi/ext-cpp && GEN=ninja make release && cd ..

# load the framework into a database, then serve it
./ext-cpp/build/release/duckdb app.db -unsigned < framework.sql
./ext-cpp/build/release/duckdb app.db -unsigned < app.sql        # demo routes
./ext-cpp/build/release/duckdb app.db -unsigned \
  -c "SELECT serve_brain(8000, 'app.db'); SELECT block_forever(0);"
```

Tests:

```bash
# oracle suite (197 checks, no server needed)
cat framework.sql test/tier1_handle_request.test.sql | ./ext-cpp/build/release/duckdb -unsigned
# extension suite
cd ext-cpp && make test
```

## Honest edges

The project keeps a ledger of where the abstraction genuinely tears instead of
hiding it ([`edges.md`](edges.md), plus current status in
[`docs/STATUS.md`](docs/STATUS.md)):

- **Inbound browser WebSockets** on the main port need further C-server work
  (radio is an outbound client; SSE is shipped and covers most push cases).
- **TLS is proxy-terminated** in v1 — same deployment answer as uvicorn's.
- **Subscriptions are at-most-once** with an in-process buffer and a per-row
  error ledger — an event-hook surface, not a durable queue.
- **Single writer** bounds write throughput (reads scale via the replica pool).
- Secrets at rest are plaintext pending `SECRET ENV` indirection
  (tracked in [`docs/SECURITY.md`](docs/SECURITY.md) open items).

## Repo map

| Path | Role |
|---|---|
| `framework.sql` | the oracle: registries, `handle_request`, all framework macros |
| `app.sql` | demo application (routes as data) |
| `ext-cpp/` | the compiled DuckDB extension (server, worker pool, DDL sugar) — submodule |
| `test/` | tier-1 oracle suite, parity harness, conformance matrix vs real FastAPI |
| `docs/FASTAPI_MOST_WANTED.md` | the scoreboard |
| `docs/SECURITY.md` | security model + audit ledger |
| `docs/STATUS.md` · `docs/BACKLOG.md` | current state, honest gaps, roadmap |
| `edges.md` | hypothesis → probe → verdict ledger of where the abstraction tears |
| `bench/` | head-to-head methodology + raw ab output |
