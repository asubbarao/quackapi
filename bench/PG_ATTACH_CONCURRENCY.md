# DuckDB `postgres` ATTACH under concurrency

Research note for the quackapi vs FastAPI bench. Question: does a naive
`ATTACH … (TYPE postgres)` open a **single** Postgres connection and serialize
all concurrent HTTP workers behind it?

**Short answer:** No — it is not a single-connection queue. DuckDB’s postgres
extension keeps an **in-memory connection pool** (default max **24** on this
16-core machine). Concurrent clients open many backends. Throughput still does
**not** scale linearly inside one quackapi process: it rises from 1→8 VUs,
plateaus by 16, and **collapses at 32 VUs** even while dozens of PG connections
sit open. Raising `pg_pool_max_connections` opens more sockets but does **not**
fix the collapse.

**Recommendation for `serve_quackapi.sh` (one line):**

```sql
LOAD postgres; SET pg_pool_max_connections=32; SET pg_connection_limit=32; ATTACH 'postgresql://…' AS pg (TYPE postgres);
```

Set the pool **before** `ATTACH` (pool options apply to newly attached DBs).
Match worker threads (32). Do not treat that as a fix for the c=32 collapse —
it only avoids an artificial pool-size ceiling if `pg_pool_acquire_mode` is
changed from the default `force`.

---

## 1. What the extension does (settings + docs)

Binary under test: DuckDB **v1.5.4**
(`/Users/aloksubbarao/personal/quackapi/build/release/duckdb`).

```text
SELECT name, value FROM duckdb_settings() WHERE starts_with(name, 'pg_') ORDER BY name;
```

Observed defaults on this host (16 threads):

| Setting | Default (this build) | Role |
|---------|----------------------|------|
| `pg_pool_max_connections` | **24** | Max cached connections per attached PG DB. Docs: `4 ≤ cpu_count×1.5 ≤ 32`. |
| `pg_connection_limit` | **24** | **Deprecated** alias of the pool max; same value. |
| `pg_connection_cache` | **true** | **Deprecated**; disable via `pg_pool_max_connections=0`. |
| `pg_pool_acquire_mode` | **`force`** | `force` = always connect, ignore pool limit; `wait` = block; `try` = fail if full. |
| `pg_pool_wait_timeout_millis` | 30000 | Wait budget when mode is `wait`. |
| `pg_pool_idle_timeout_millis` | 60000 | Idle eviction (with reaper). |
| `pg_pool_enable_reaper_thread` | true | Background idle/lifetime cleanup. |
| `pg_pool_enable_thread_local_cache` | false | Pin conns to threads (can starve pool). |
| `pg_use_ctid_scan` | true | Parallel table scans can use multiple pool conns. |
| `pg_pages_per_task` | 1000 | Work chunking for parallel scans. |

Docs (DuckDB current): [PostgreSQL Extension Connection Pool](https://duckdb.org/docs/current/core_extensions/postgres/connection_pool.html).

Implications:

- ATTACH is **pooled**, not one global socket for the life of the process.
- Default `force` means the published `max_connections` is a **soft** cache size;
  parallel work / concurrent acquire can open past it.
- Parallel **scans** of one table can fan out up to `threads`, further capped by
  the pool for extra workers.
- Global `SET pg_pool_*` only affects databases **attached after** the SET.
  Already-attached catalogs need `postgres_configure_pool(...)`.

`LOAD postgres` must precede any `SET pg_pool_*` (settings live in the
extension catalog).

---

## 2. Measurement method

Two shapes:

| Shape | What it models |
|-------|----------------|
| **A. quackapi probe** (one DuckDB process, httplib worker pool 32, new `Connection` per request — same pattern as `quackapi_server.cpp`) | Real bench stack: concurrent HTTP → concurrent DuckDB connections → shared ATTACH pool |
| **B. multiproc CLI** (`xargs -P`, one process per worker, each ATTACH + N PK lookups) | Many independent DuckDB processes; proves PG accepts many clients and multiproc scales |

Probe routes hit **live** `pg.bench_rows` only (no local materialization):

```sql
ATTACH 'postgresql://admin:password@127.0.0.1:6432/quackbench' AS pg (TYPE postgres);
CREATE ROUTE item GET '/items/:id' AS
  SELECT id, name FROM pg.bench_rows WHERE id = $id::INTEGER;
```

Load: k6 `GET /items/{random 1..100000}`, duration 6–8s, VUs ∈ {1,8,16,32}.
Postgres sampler (~50 ms):  
`count(*)` / `state` from `pg_stat_activity` where `datname='quackbench'`.

Tuned variant: `SET pg_pool_max_connections=64; SET pg_connection_limit=64;`
before ATTACH (port 8011). Defaults on 8010.

---

## 3. Results

### 3a. Single process (quackapi + ATTACH) — primary

| Config | VUs | http_reqs/s | avg latency | peak PG client backends | peak `active` |
|--------|-----|-------------|-------------|-------------------------|---------------|
| defaults (pool 24 / force) | 1 | **477** | 2.1 ms | 27 | 1 |
| defaults | 8 | **1148** | 6.9 ms | 27 | 2 |
| defaults | 16 | **1127** | 14.1 ms | 27 | 2 |
| defaults | 32 | **156** | 201 ms | 29 | 3 |
| tuned (pool 64) | 1 | **468** | 2.1 ms | 27 | 1 |
| tuned | 8 | **1112** | 7.1 ms | 34 | 1 |
| tuned | 16 | **1125** | 14.1 ms | 42 | 2 |
| tuned | 32 | **136** | 234 ms | **52** | 2 |

Scaling vs c=1 (defaults): **×2.4 at 8**, **×2.4 at 16**, **×0.33 at 32**.

Not a single-connection picture: tens of backends appear. At c=32, latency
explodes while `active` on Postgres stays ~1–3 — most time is **not** spent
with many queries executing in PG. Pool size is not the binding constraint:
raising max to 64 grows idle sockets (29→52) and makes the collapse slightly
worse, not better.

Checks: 100% HTTP 200 on all runs (lookups succeed; the issue is throughput /
latency, not errors).

### 3b. Multi-process CLI ATTACH (clean residual conns)

Each process: ATTACH + 500 correlated single-row lookups via
`pg.bench_rows WHERE id = …`.

| Processes | Lookups | Wall s | QPS (lookups/s) | peak total PG conns | peak active |
|-----------|---------|--------|-----------------|---------------------|-------------|
| 1 | 500 | 0.071 | **7083** | 3 | 1 |
| 8 | 4000 | 0.107 | **37441** | 10 | 6 |
| 32 | 16000 | 0.246 | **64926** | 28 | 4 |

Rough scaling vs c=1: **×5.3 at 8**, **×9.2 at 32** — sublinear but **still
increasing**, and connection count ≈ process count. Absolute QPS is not
comparable to HTTP (one DuckDB query plan vs one request per lookup), but the
direction answers the multiproc question: ATTACH does **not** pin the machine
to one PG backend.

(Earlier multiproc spot-check with 200 lookups/worker: c=1 → 2763 QPS, c=8 →
14872, c=32 → 26421 — same shape.)

---

## 4. Does any setting “fix” serialization?

| Change | Effect |
|--------|--------|
| Raise `pg_pool_max_connections` / `pg_connection_limit` to 64 | More idle backends under load; **no** recovery of c=32 RPS |
| Default `pg_pool_acquire_mode=force` | Already allows opening past the soft max (seen: 29 conns with max 24) |
| Leave pool at default 24 with force | Enough for ≤24 concurrent acquires; still collapse at 32 VUs for other reasons |

There is **no** single SET that turns the quackapi c=32 collapse into linear
scaling in these measurements. The naive “one connection” theory is **false**;
the “pool too small” theory is **insufficient**.

Likely remaining bottlenecks (not fully isolated here): DuckDB multi-connection
contention inside one process, per-request `Connection` construction in
quackapi, planner/scanner overhead for ATTACH point lookups, or lock domains
other than the PG socket count. Separating those is out of scope for this note.

---

## 5. Implications for the published bench

1. **Honesty rule** (every request hits Postgres) is compatible with ATTACH —
   do not pre-materialize `bench_rows` into DuckDB.
2. **Do not** claim “quackapi loses because ATTACH is single-threaded on one
   PG connection.” `pg_stat_activity` shows many backends.
3. **Do** expect sublinear HTTP scaling and a sharp degradation near 32
   concurrent ATTACH point-lookups in one DuckDB process. Report VUs carefully;
   8–16 is the more informative concurrent band on this machine for item GETs.
4. **Serve script:** `LOAD postgres` → `SET pg_pool_max_connections=32` (and
   legacy `pg_connection_limit=32`) → `ATTACH …` so the pool is at least as
   wide as `QUACKAPI_DEFAULT_WORKER_THREADS` / FastAPI `max_size=32`. Keep
   `pg_pool_acquire_mode` at default `force` unless you intentionally want
   hard capping (`wait`/`try`).
5. FastAPI’s `psycopg_pool.ConnectionPool(max_size=32)` is a fair peer sizing
   target; DuckDB’s pool is the analogous knob.

---

## 6. Raw command highlights (verbatim)

### Settings probe

```text
│ pg_connection_cache                   │ true     │
│ pg_connection_limit                   │ 24       │
│ pg_pool_acquire_mode                  │ force    │
│ pg_pool_max_connections               │ 24       │
│ threads                               │ 16       │
```

### k6 defaults (port 8010) — excerpts

```text
# VUs=1, 6s
http_reqs......................: 2862   476.939747/s
http_req_duration..............: avg=2.06ms …
peak_total=27 peak_active=1

# VUs=8, 6s
http_reqs......................: 6895   1148.197588/s
http_req_duration..............: avg=6.92ms …
peak_total=27 peak_active=2

# VUs=16, 6s
http_reqs......................: 6774   1127.240941/s
http_req_duration..............: avg=14.08ms …
peak_total=27 peak_active=2

# VUs=32, 6s
http_reqs......................: 952    156.421034/s
http_req_duration..............: avg=201.24ms … med=221.48ms …
peak_total=29 peak_active=3
```

### k6 tuned pool=64 (port 8011) — c=32 collapse still present

```text
http_reqs......................: 828    136.116958/s
http_req_duration..............: avg=233.73ms …
peak_total=52 peak_active=2
```

### Multiproc CLI (clean)

```text
multiproc c=1 total=500 elapsed=0.071s qps=7082.9 peak_total=3 peak_active=1
multiproc c=8 total=4000 elapsed=0.107s qps=37441.3 peak_total=10 peak_active=6
multiproc c=32 total=16000 elapsed=0.246s qps=64926.1 peak_total=28 peak_active=4
```

Artifacts under `bench/results/pg_attach_probe/` (k6 quiet runs, samplers,
probe SQL). Probe servers on :8010/:8011 were stopped after measurement.

---

## 7. Bottom line

| Claim | Verdict |
|-------|---------|
| ATTACH uses one PG connection and serializes everyone | **False** (pool; many backends observed) |
| Throughput scales linearly with concurrency in one DuckDB/quackapi process | **False** (plateau ~8–16 VUs, collapse at 32) |
| Raising `pg_pool_max_connections` restores linear scale | **False** (more conns, same collapse) |
| Multiproc independent ATTACHes can drive many PG backends and gain QPS | **True** |

Serve script should still size the pool to the worker count before ATTACH; the
bench must not confuse “pool too small” with “framework speed,” and must not
“fix” fairness by caching rows out of Postgres.
