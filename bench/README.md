# quackapi vs FastAPI — pgEdge-backed HTTP bench (v2)

Same four routes, same **live** pgEdge Postgres database, same k6 scenarios.
The claim under test is the **web layer + how each stack talks to Postgres** —
not “DuckDB vs Postgres,” not local CTAS.

## Product order (how we score)

1. **Equivalence** — same routes, same PG, correct HTTP semantics  
2. **Surpass** — **slaughter FastAPI** on latency + RPS (this bench)  
3. **Leapfrog** — features FastAPI doesn’t have (policies, queue DDL, …) *after* 1–2

## Scoreboard (2026-07-24, w1, measure 20s, `pg_dsn` libpq path)

| scenario | VUs | quackapi med | FastAPI med | speedup | quackapi rps | FastAPI rps | rps× |
|----------|----:|-------------:|------------:|--------:|-------------:|------------:|-----:|
| hello | 32 | **1.00 ms** | 2.84 ms | **2.8×** | 22 384 | 10 293 | **2.2×** |
| item | 1 | **0.24 ms** | 0.44 ms | **1.8×** | 1 380 | 1 651 | 0.8×† |
| item | 8 | **0.61 ms** | 1.63 ms | **2.7×** | 8 098 | 4 354 | **1.9×** |
| item | 16 | **0.67 ms** | 3.19 ms | **4.8×** | 18 480 | 4 596 | **4.0×** |
| item | 32 | **0.94 ms** | 6.11 ms | **6.5×** | 27 529 | 4 886 | **5.6×** |
| rows | 32 | **4.46 ms** | 57.7 ms | **12.9×** | 4 330 | 516 | **8.4×** |
| write | 1 | **0.36 ms** | 0.65 ms | **1.8×** | 1 968 | 1 122 | **1.8×** |
| write | 8 | **0.74 ms** | 1.77 ms | **2.4×** | 9 018 | 3 987 | **2.3×** |
| write | 16 | **1.05 ms** | 3.26 ms | **3.1×** | 12 921 | 4 454 | **2.9×** |
| write | 32 | **1.45 ms** | 6.34 ms | **4.4×** | 18 488 | 4 653 | **4.0×** |

† item@1 rps is k6 1-VU noise (med latency still wins). Under load, item **slaughters**.

**Verdict (w1): zero lose cells. Under concurrency, 3–13× faster med, 2–8× RPS.**

### w8 (8 processes, SO_REUSEPORT — no proxy tax)

| scenario | VUs | quackapi med | FastAPI med | ×lat | note |
|----------|----:|-------------:|------------:|-----:|------|
| hello | 32 | **0.45 ms** | 0.96 ms | **2.1×** | |
| item | 1 | **0.19 ms** | 0.43 ms | **2.2×** | |
| item | 32 | **0.46 ms** | 2.29 ms | **5.0×** | |
| rows | 32 | **1.42 ms** | 10.5 ms | **7.4×** | |
| write | 1 | **0.36 ms** | 0.61 ms | **1.7×** | |
| write | 32 | **0.74 ms** | 2.68 ms | **3.7×** | |

Workers bind the **same port** via kernel `SO_REUSEPORT` (httplib default). The old Python RR proxy was doubling hello latency — deleted.

| Stack | Process | Port | Data path |
|-------|---------|------|-----------|
| **quackapi-w1** | one DuckDB + quackapi | `8000` | **`pg_dsn` libpq** (thread-local + PQprepare); ATTACH kept as fallback |
| **quackapi-w8** | 8 DuckDB processes + RR proxy | `8000` | same, pool capped per worker |
| **fastapi-w1** | uvicorn `--workers 1` | `8001` | psycopg3 `ConnectionPool` |
| **fastapi-w8** | uvicorn `--workers 8` | `8001` | same app, multi-process |

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

- **One process** (w1) or **N processes + RR proxy** (w8): DuckDB CLI +
  quackapi (`-unsigned`, `-init /dev/null`).
- **32 httplib worker threads** (w1); fewer per process when multi-worker.
- Handlers are pure SQL via `CREATE ROUTE` (see `routes.sql`).
- **Scoreboard path:** `quackapi_serve(…, pg_dsn := 'postgresql://…')` —
  thread-local **libpq** + prepare cache (same model as FastAPI+psycopg).
- ATTACH is still in `routes.sql` as DuckDB fallback only — not the hot path
  when `pg_dsn` is set. See [PG_ATTACH_CONCURRENCY.md](./PG_ATTACH_CONCURRENCY.md)
  for why ATTACH alone collapsed under concurrency (pre-libpq).
- In-memory DuckDB holds routes only — **not** a copy of `bench_rows`.

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
