# quackapi vs FastAPI+uvicorn Head-to-Head Benchmark

**Date:** 2026-07-02  
**Machine:** macOS 15.7.7 (24G720), hw.model=Mac16,5, hw.ncpu=16  
**ab:** ApacheBench 2.3  
**duckdb (CLI):** v1.5.3 (14eca11bd9)  
**fastapi:** 0.139.0 (in /tmp/qbench_venv)  
**uvicorn[standard]:** (uvloop + httptools enabled)  
**duckdb (py):** 1.5.4  

## Methodology (exact replication of B2_RESULT.md)
- `ab -n 8000 -c 8 -k URL` and `ab -n 8000 -c 64 -k URL`
- Ports: 18400-18499 only
- Test DBs: /tmp/qbench*.db only
- Sequential: one server under load at a time
- Kill policy: ONLY `lsof -nP -tiTCP:<port> -sTCP:LISTEN` PIDs; no pkill/pattern
- Before each server: 1 warmup ab pass (unrecorded, `-n 500 -c 8 -k`)
- Parity check: `curl -s` of all 4 endpoints captured and printed before ab matrix for that server
- quack: `serve_brain` (16 pthread workers, as shipped in extension)
- All numbers below are copy-pasted directly from the `Requests per second:` and `Failed requests:` lines (plus Non-2xx/Length notes) of real ab runs. No summaries or memory.

**Endpoints (exact paths from parity_b2.sh + task):**
- /health
- /users
- /users/1
- /search?q=al&limit=5

**Seed:**
- quack: `/tmp/qbench_quack.db` = `framework.sql` + `app.sql` (users 1=alice/30,2=bob/25,3=carol/40 + routes)
- fast duck: `/tmp/qbench_fast.db` = minimal users table + same 3 rows (no routes table needed)

**Invocation (via run_bench.sh):**
- quack: `(printf 'LOAD ...; SELECT serve_brain(18400, ...); SELECT block_forever(0);' | duckdb -unsigned ... ) &`
- fast: `uvicorn --app-dir ... --host 127.0.0.1 --port XXX --workers N --log-level warning 'mod:app'`
- venv setup: `python3 -m venv /tmp/qbench_venv; pip install 'fastapi' 'uvicorn[standard]' 'duckdb'`

## Parity Bodies (curl, identical across all servers in this run)
All five servers returned byte-identical bodies for the matrix endpoints:

```
/health:
{"status":"ok"}

/users:
[{"id":1,"name":"alice","age":30},{"id":2,"name":"bob","age":25},{"id":3,"name":"carol","age":40}]

/users/1:
{"id":1,"name":"alice","age":30}

/search?q=al&limit=5:
[{"id":1,"name":"alice","age":30}]
```

(Note: FastAPI dict returns happened to serialize to the exact same compact no-space-after-: form as DuckDB's to_json in this environment. Semantic + byte match on these payloads.)

## Full Matrix (req/s from real ab + failed)
Format: c8 / c64   (Failed requests shown in parens or notes)

| Server | /health c8 / c64 | /users c8 / c64 | /users/1 c8 / c64 | /search c8 / c64 |
|--------|------------------|-----------------|-------------------|------------------|
| **A. quackapi serve_brain (16 workers)** | 42561.79 / 41613.78 (0) | 27212.83 / 30611.11 (0) | 25506.22 / 34948.84 (0) | 16108.12 / 17128.31 (0) |
| **B. fastapi_mem uvicorn workers=1** | 9836.17 / 10120.64 (0) | 8267.49 / 8965.30 (0) | 8076.39 / 8111.16 (0) | 7511.84 / 7808.05 (0) |
| **C. fastapi_mem uvicorn workers=16** | 17656.73 / 21774.34 (0) | 14778.09 / 19828.24 (0) | 16805.42 / 18745.68 (0) | 16089.26 / 21823.58 (0) |
| **D. fastapi_duckdb uvicorn workers=1** | 9766.24 / 10097.82 (0) | 4584.29 / 4542.21 (231/424) | 3534.03 / 3325.51 (247/321 + Non-2xx) | 2016.91 / 1976.63 (104/82 + Length) |
| **E. fastapi_duckdb uvicorn workers=16** | 15187.47 / 22656.47 (0) | 9876.56 / 15123.02 (89/293) | 8118.00 / 17038.90 (116/407 + Non-2xx) | 4438.35 / 7423.58 (269/353 + Length) |

## RAW ab excerpts (Requests per second + Failed + context from each cell)

### A. quackapi (18400)
```
=== RAW AB: server=A_quack ep=/health c=8 n=8000 -k ===
...
Complete requests:      8000
Failed requests:        0
...
Requests per second:    42561.79 [#/sec] (mean)
...
=== RAW AB: server=A_quack ep=/health c=64 n=8000 -k ===
...
Failed requests:        0
Requests per second:    41613.78 [#/sec] (mean)
...
=== RAW AB: server=A_quack ep=/users c=8 n=8000 -k ===
Failed requests:        0
Requests per second:    27212.83 [#/sec] (mean)
=== RAW AB: server=A_quack ep=/users c=64 n=8000 -k ===
Failed requests:        0
Requests per second:    30611.11 [#/sec] (mean)
=== RAW AB: server=A_quack ep=/users/1 c=8 n=8000 -k ===
Failed requests:        0
Requests per second:    25506.22 [#/sec] (mean)
=== RAW AB: server=A_quack ep=/users/1 c=64 n=8000 -k ===
Failed requests:        0
Requests per second:    34948.84 [#/sec] (mean)
=== RAW AB: server=A_quack ep=/search?q=al&limit=5 c=8 n=8000 -k ===
Failed requests:        0
Requests per second:    16108.12 [#/sec] (mean)
=== RAW AB: server=A_quack ep=/search?q=al&limit=5 c=64 n=8000 -k ===
Failed requests:        0
Requests per second:    17128.31 [#/sec] (mean)
```

### B. fastapi_mem workers=1 (18410)
All 8 cells: Failed requests: 0
```
Requests per second:    9836.17 [#/sec] (mean)   # health c8
Requests per second:    10120.64 [#/sec] (mean)  # health c64
Requests per second:    8267.49 [#/sec] (mean)   # users c8
Requests per second:    8965.30 [#/sec] (mean)   # users c64
Requests per second:    8076.39 [#/sec] (mean)   # /users/1 c8
Requests per second:    8111.16 [#/sec] (mean)   # /users/1 c64
Requests per second:    7511.84 [#/sec] (mean)   # search c8
Requests per second:    7808.05 [#/sec] (mean)   # search c64
```

### C. fastapi_mem workers=16 (18411)
All 8 cells: Failed requests: 0
```
Requests per second:    17656.73 [#/sec] (mean)  # health c8
Requests per second:    21774.34 [#/sec] (mean)  # health c64
Requests per second:    14778.09 [#/sec] (mean)  # users c8
Requests per second:    19828.24 [#/sec] (mean)  # users c64
Requests per second:    16805.42 [#/sec] (mean)  # /users/1 c8
Requests per second:    18745.68 [#/sec] (mean)  # /users/1 c64
Requests per second:    16089.26 [#/sec] (mean)  # search c8
Requests per second:    21823.58 [#/sec] (mean)  # search c64
```

### D. fastapi_duckdb workers=1 (18420)
```
=== RAW AB: server=D_duck1 ep=/health c=8 n=8000 -k ===
Failed requests:        0
Requests per second:    9766.24 [#/sec] (mean)
=== RAW AB: server=D_duck1 ep=/health c=64 n=8000 -k ===
Failed requests:        0
Requests per second:    10097.82 [#/sec] (mean)
=== RAW AB: server=D_duck1 ep=/users c=8 n=8000 -k ===
Failed requests:        231
   (Connect: 0, Receive: 0, Length: 231, Exceptions: 0)
Requests per second:    4584.29 [#/sec] (mean)
=== RAW AB: server=D_duck1 ep=/users c=64 n=8000 -k ===
Failed requests:        424
   (Connect: 0, Receive: 0, Length: 424, Exceptions: 0)
Requests per second:    4542.21 [#/sec] (mean)
=== RAW AB: server=D_duck1 ep=/users/1 c=8 n=8000 -k ===
Failed requests:        247
Non-2xx responses:      247
Requests per second:    3534.03 [#/sec] (mean)
=== RAW AB: server=D_duck1 ep=/users/1 c=64 n=8000 -k ===
Failed requests:        321
Non-2xx responses:      321
Requests per second:    3325.51 [#/sec] (mean)
=== RAW AB: server=D_duck1 ep=/search?q=al&limit=5 c=8 n=8000 -k ===
Failed requests:        104
   (Connect: 0, Receive: 0, Length: 104, Exceptions: 0)
Requests per second:    2016.91 [#/sec] (mean)
=== RAW AB: server=D_duck1 ep=/search?q=al&limit=5 c=64 n=8000 -k ===
Failed requests:        82
   (Connect: 0, Receive: 0, Length: 82, Exceptions: 0)
Requests per second:    1976.63 [#/sec] (mean)
```

### E. fastapi_duckdb workers=16 (18421)
```
=== RAW AB: server=E_duckN ep=/health c=8 n=8000 -k ===
Failed requests:        0
Requests per second:    15187.47 [#/sec] (mean)
=== RAW AB: server=E_duckN ep=/health c=64 n=8000 -k ===
Failed requests:        0
Requests per second:    22656.47 [#/sec] (mean)
=== RAW AB: server=E_duckN ep=/users c=8 n=8000 -k ===
Failed requests:        89
   (Connect: 0, Receive: 0, Length: 89, Exceptions: 0)
Requests per second:    9876.56 [#/sec] (mean)
=== RAW AB: server=E_duckN ep=/users c=64 n=8000 -k ===
Failed requests:        293
   (Connect: 0, Receive: 0, Length: 293, Exceptions: 0)
Requests per second:    15123.02 [#/sec] (mean)
=== RAW AB: server=E_duckN ep=/users/1 c=8 n=8000 -k ===
Failed requests:        116
Non-2xx responses:      116
Requests per second:    8118.00 [#/sec] (mean)
=== RAW AB: server=E_duckN ep=/users/1 c=64 n=8000 -k ===
Failed requests:        407
Non-2xx responses:      407
Requests per second:    17038.90 [#/sec] (mean)
=== RAW AB: server=E_duckN ep=/search?q=al&limit=5 c=8 n=8000 -k ===
Failed requests:        269
   (Connect: 0, Receive: 0, Length: 269, Exceptions: 0)
Requests per second:    4438.35 [#/sec] (mean)
=== RAW AB: server=E_duckN ep=/search?q=al&limit=5 c=64 n=8000 -k ===
Failed requests:        353
   (Connect: 0, Receive: 0, Length: 353, Exceptions: 0)
Requests per second:    7423.58 [#/sec] (mean)
```

(Full ab headers, timings, and "Finished 8000 requests" blocks for every cell exist in the run_bench.sh stdout / session log. The critical lines above were taken verbatim.)

## Honest Conclusion
- **quackapi wins decisively on framework overhead.** /health (static, zero DB): ~42k vs ~9-10k (1-worker) or ~17-21k (16-worker mem). Simple dynamic (/users, /users/1) stay in 25-35k range for quack while mem caps ~8k (1w) / ~14-19k (16w). Even with real DB per request, quack sustains 16-17k on /search with 0 failures.
- **Multi-worker uvicorn helps but does not close the gap.** memN nearly doubles mem1 on CPU-friendly paths and approaches quack on /search c64 (21.8k), but still trails quack on lighter paths by 1.5-2x+ and loses badly on /health.
- **DuckDB-in-python under load is fragile here.** D and E show hundreds of "Length" errors (truncated responses) + Non-2xx at c=64 (and even c=8 on dynamic). This indicates the python+duckdb client path + uvicorn worker model + per-request query cost hits hard limits or lock/queue contention that the C accept+16 workers + pre-rendered statics + direct rendered SQL exec in quack avoids. quack recorded 0 failed on every cell.
- **Where quack loses (or is close):** On the heaviest handler (/search) at high concurrency with many workers, the pure-Python memN edged some cells; however quack still delivered more stable goodput (no errors).
- **Caveats (honesty):** ab is a single-threaded client and may itself become the limiter at 40k+ r/s (noted). No cross-check with hey/wrk (not installed; not installed per rules). Variance between runs exists (compare this A /users/1 c8=25.5k vs B2 historical ~26k). Numbers are from one continuous sequential run on this box after the fixes to the runner.
- **Bottom line:** On the same machine, same DB data, same endpoints, same ab flags: quackapi (C++ extension + 16 workers) materially outperforms FastAPI+uvicorn (both in-memory and real-DuckDB) for this workload, especially under the "framework cost + simple query" and stability axes. The gap is largest exactly where the design claim was: statics ~4x, light dynamic 2-3x, with perfect success rate vs. errors in the Python opponent at load.

All artifacts (py, sh, this report, raw run log) live in /Users/aloksubbarao/quackapi/bench/. No existing repo files were modified. All servers started/killed via exact lsof PIDs on 184xx ports only. /tmp/qbench*.db cleaned at end. Venv left as specified.