# quackapi vs FastAPI — pgEdge-backed HTTP bench (v2)

Same four routes, same **live** pgEdge Postgres database, same k6 scenarios,
three stack configurations. The claim under test is the **web layer + how each
stack talks to Postgres** — not “DuckDB vs Postgres.”

| Stack | Process | Port | Data path |
|-------|---------|------|-----------|
| **quackapi** | one DuckDB CLI + quackapi extension | `8000` | SQL routes (`CREATE ROUTE`) over `ATTACH … (TYPE postgres)` |
| **fastapi-w1** | uvicorn `--workers 1` + FastAPI | `8001` | psycopg3 `ConnectionPool` |
| **fastapi-w8** | uvicorn `--workers 8` + FastAPI | `8001` | same app / pool settings, multi-process workers |

## Iron rule (fairness)

**Every request that touches data must reach Postgres.** No stack may
pre-materialize `bench_rows` into a local DuckDB table, a Python dict, or any
in-process cache. Both stacks query the **same** pgEdge node (`n1:6432`,
database `quackbench`).

## Data plane — pgEdge Postgres 17

Already seeded (do **not** drop or reseed):

| Table | Rows / role |
|-------|-------------|
| `bench_rows` | 100 000 rows: `id` int PK, `name` text, `value` numeric, `ts` timestamp |
| `bench_writes` | append target: `id` bigint, `note` text, `ts` default `current_timestamp` |

DSN:

```text
postgresql://admin:password@127.0.0.1:6432/quackbench
```

If Postgres is down: `podman start pgedge-n1`.

## Stack details

### quackapi (port 8000)

- **One process**: prebuilt DuckDB CLI + quackapi extension (`-unsigned`,
  `-init /dev/null`).
- **32 httplib worker threads** (`QUACKAPI_DEFAULT_WORKER_THREADS`).
- Handlers are pure SQL via `CREATE ROUTE` (see `routes.sql`).
- Data: DuckDB **`postgres` ATTACH** to pgEdge — pooled; default
  `pg_pool_max_connections` is **24** on this machine (see
  [PG_ATTACH_CONCURRENCY.md](./PG_ATTACH_CONCURRENCY.md)).
- In-memory DuckDB holds routes + the ATTACH only — **not** a copy of
  `bench_rows`.

### fastapi (port 8001)

- FastAPI + uvicorn; handlers are plain `def` (Starlette/AnyIO threadpool
  size **32**, matched to quackapi workers).
- **psycopg3 `ConnectionPool`** to the same DSN (`min_size=2`,
  `max_size=32`).
- Measured at **1 and 8 uvicorn workers** (`fastapi-w1`, `fastapi-w8`) so
  multi-process scaling is visible next to single-process quackapi.

### Why item is VUS-swept

`GET /items/:id` is swept across **VUs 1 / 8 / 16 / 32** because quackapi’s
ATTACH path was measured to **collapse between 16 and 32 VUs**
(~1148 rps @8 → ~156 rps @32, with ~27 PG backends open but only 1–3 active).
Full write-up: [PG_ATTACH_CONCURRENCY.md](./PG_ATTACH_CONCURRENCY.md).

`write` is swept across **1 / 8 / 16 / 32 / 64**. `hello` and `rows` run at a
single VUs level (default 32).

## Prerequisites

- pgEdge up: `podman start pgedge-n1` (port **6432**)
- Prebuilt (do **not** rebuild):
  - duckdb CLI: `/Users/aloksubbarao/personal/quackapi/build/release/duckdb`
  - extension: `/Users/aloksubbarao/personal/quackapi/build/release/extension/quackapi/quackapi.duckdb_extension`
- k6: `/opt/homebrew/bin/k6`
- `uv` for the FastAPI venv at `bench/.venv` (`uvicorn` / `fastapi` /
  `psycopg[binary]` / `psycopg_pool` are **not** assumed global)

## How to run

```bash
cd /Users/aloksubbarao/personal/quackapi-bench

# Ensure pgEdge is answering
podman start pgedge-n1
PGPASSWORD=password /Applications/Postgres.app/Contents/Versions/latest/bin/psql \
  -h 127.0.0.1 -p 6432 -U admin -d quackbench -c 'SELECT count(*) FROM bench_rows;'

# Full matrix (all stacks, all scenarios, item + write VUS sweeps)
bash bench/run.sh

# Subsets (exact CLI tokens depend on run.sh; common shape)
bash bench/run.sh quackapi
bash bench/run.sh hello item
```

`run.sh` (serial stacks by design — never concurrent on shared cores):

1. Clears `bench/results/`
2. Records machine + versions into `bench/results/env.txt`
3. For each stack: start serve script → poll `/hello` until ready → run k6 →
   kill server → wait for port release
4. After write runs, writes
   `<stack>__write__vus<N>__rowcheck.txt` (`pg_rows k6_successful_reqs`)
5. Prints the comparison table via `bench/report.sql`

Re-print the table later:

```bash
cd /Users/aloksubbarao/personal/quackapi-bench/bench
/Users/aloksubbarao/personal/quackapi/build/release/duckdb -init /dev/null -c ".read report.sql"
```

### Env knobs

| Variable | Default | Meaning |
|----------|---------|---------|
| `WARMUP_DURATION` | `5s` | Ramp 0 → VUs (excluded from reported metrics) |
| `MEASURE_DURATION` | `20s` | Steady-state window that populates the report |
| `DEFAULT_VUS` | `32` | VUs for hello / rows |
| `ITEM_VUS_LIST` | `1 8 16 32` | Concurrency sweep for item |
| `WRITE_VUS_LIST` | `1 8 16 32 64` | Concurrency sweep for write |
| `ROWS_N` | `1000` | `GET /rows?n=` payload size |
| `READY_TIMEOUT_SEC` | `90` | Max wait for stack ready |

## Stage durations (warmup excluded from numbers)

Every scenario uses the same two k6 stages:

1. **warmup** — `ramping-vus`, `0 → VUs` over **5s**, tag `stage=warmup`
2. **measure** — `constant-vus` at **VUs** for **20s**, tag `stage=measure`

`report.sql` reads only measure-stage submetrics:

- `http_reqs{stage:measure}`
- `http_req_duration{stage:measure}`
- `checks{stage:measure}`

## Scenarios

| Script | Request | What it measures |
|--------|---------|------------------|
| `scenarios/hello.js` | `GET /hello` | Routing + JSON ceiling (no DB) |
| `scenarios/item.js` | `GET /items/<id>` | PK lookup via path param (PG) |
| `scenarios/rows.js` | `GET /rows?n=<ROWS_N>` | Multi-row materialization + JSON |
| `scenarios/write.js` | `POST /write` `{"id","note"}` | Concurrent INSERT under load |

Checks assert **response shape**, not byte-equality of timestamps. DuckDB and
psycopg format timestamps differently (`"2026-01-01 00:00:00"` vs
`"2026-01-01T00:00:00"`); both are accepted. Shape = 2xx + keys present +
typed ids where applicable. A fast-but-erroring run shows a high
`*_check_fail` rate — do not trust RPS when that column is non-zero.

### Results file naming

```text
bench/results/<stack>__hello.json
bench/results/<stack>__rows.json
bench/results/<stack>__item__vus<N>.json          # N in 1 8 16 32
bench/results/<stack>__write__vus<N>.json         # N in 1 8 16 32 64
bench/results/<stack>__write__vus<N>__rowcheck.txt  # "<pg_rows> <k6_ok>"
bench/results/<stack>__server.log
bench/results/env.txt
bench/results/fastapi_versions.txt
```

`<stack>` is exactly one of: `quackapi`, `fastapi-w1`, `fastapi-w8`.

## What the comparison table means

`report.sql` emits one row per scenario label (`hello`, `rows`, `item@vusN`,
`write@vusN`) with columns for each stack:

| Column family | Meaning |
|---------------|---------|
| `*_rps` | Measure-stage request rate |
| `*_p50_ms` / `*_p95_ms` / `*_p99_ms` | Measure-stage `http_req_duration` percentiles (ms) |
| `*_check_fail` | Fraction of k6 checks that failed (0 = all shape/status checks passed) |
| `*_pg_rows` / `*_k6_ok` | Write rowcheck only: rows counted in Postgres vs k6 successful requests |

## What this does **not** measure

- **Native TLS / HTTPS on quackapi.** Inbound is DuckDB’s vendored **httplib
  without OpenSSL** — **plaintext HTTP only**. Real deployments terminate TLS
  at a reverse proxy (nginx, caddy, cloud LB); proxy and crypto cost are not
  in this table.
- **spock multi-master replication.** Single pgEdge node (`n1:6432`) only;
  cross-node replication lag / conflict paths are not exercised.
- **Real network latency.** k6 and servers are on localhost (same Mac).
- Auth, middleware, rate limits, compression, HTTP/2, websockets.
- Cold-start / process boot time (servers are warmed before measure).
- Correctness under partial failure, schema evolution, or ops tooling.

## Routes under test

Identical contracts on every stack:

1. `GET  /hello` → `[{"msg":"world"}]` (no DB)
2. `GET  /items/:id` → `[{"id":…,"name":…}]` from `bench_rows` by id;
   non-integer id → **HTTP 422**
3. `GET  /rows?n=<int>` → JSON array of `n` objects `{id,name,value,ts}` from
   `bench_rows`
4. `POST /write` body `{"id":<int>,"note":"<str>"}` → `INSERT` into
   `bench_writes`, return `[{"id":…}]`
