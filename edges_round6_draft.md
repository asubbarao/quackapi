# Round 6 — the concurrent micro-query wall (DRAFT)

**DRAFT — pending merge into edges.md.** All numbers and stacks below are taken verbatim from the four artifacts: `bench/BENCH_HEADTOHEAD.md`, `ext-cpp/B4_RESULT.md`, `ext-cpp/PROFILE_SEARCH.md`, and `ext-cpp/B5_RESULT.md`. No other runs or edits were performed.

## 10. Concurrent micro-queries against one DuckDB instance — **DEFEATED** (not fixable by config)

**Hypothesis:** "many concurrent tiny OLTP-ish queries against one DuckDB instance scale linearly with workers."

The mental model was simple: the server already runs 16 persistent worker connections (brain.cpp:450), each with `SET threads=1`, each calling `duckdb_query` (or the ddb wrapper) on its own connection for a handler that is a few-millisecond point query. If one connection sustains ~3.3k req/s serial, 16 workers should deliver close to 16× (or at least the linear fraction permitted by the machine).

**Probe (the chain of measurement).**

1. The loss cell in the head-to-head (BENCH_HEADTOHEAD.md:60):

   | Server | /search c8 / c64 |
   |--------|------------------|
   | A. quackapi (16 workers) | 16108.12 / 17128.31 (0) |
   | C. fastapi_mem uvicorn workers=16 | 16089.26 / 21823.58 (0) |

   quackapi loses only this one cell, and only at c64, only against the in-memory FastAPI variant that does **no database work at all** (pure Python dict lookup + json response).

2. B4 gate (B4_RESULT.md) was supposed to kill the parse/bind/plan tax. It did not:

   ```
   string_path:  ... 3471.5 req/s
   prepared_path: ... 3637.6 req/s   # +4.6% (N=100k)
   ...
   string_path: ... 3531.7 req/s
   prepared_path: ... 3624.1 req/s   # +2.5% (N=200k)
   ```
   Gate: <15% → STOP, no source change. But the micro also exposed the real serial floor: **one connection running the /search handler shape does ~3.3–3.5k req/s**. 16 workers were only delivering ~17k in the server (~4.9×), not the 50k+ the naive model predicted.

3. PROFILE_SEARCH.md sampled the live 16-worker server under sustained /search load (c64 ab, port 18460 only, lsof PID only).

   - Workers are **not** waiting on the C queue/cond (g_qm/g_qcv samples are tens, not thousands).
   - C layer before duckdb_query (header parse, quack_route, splice, write) is near-zero attribution.
   - Dominant frames inside every worker's samples:

     ```
     duckdb_query → ClientContext::Query → ... → Executor::ExecuteTask
       → PipelineExecutor::PipelineExecutor
       → ThreadContext::ThreadContext
       → LogManager::CreateLogger → std::mutex::lock → __psynch_mutexwait
       ... plus OperatorProfiler::OperatorProfiler ctor + operator new / nanov2_malloc
     ```

   - Allocator samples under 10s load: ~21 609 `nanov2|malloc|new` for /search vs ~3 370 for /users/1 (higher throughput, simpler handler).
   - Dual-ab on /search summed to the same ~17.1k ceiling as single ab; process CPU under load was 550–1100% (real multi-core, not serialized to 1–4).
   - Conclusion from raw stacks: the workers are all busy **inside DuckDB exec**; the per-pipeline setup and at least one global mutex per task are the tax.

4. B5_RESULT.md: every knob DuckDB exposes for a 16-thread microbench (exact same /search-shaped work, 6 distinct literal texts cycled, full result extraction):

   - Current baseline (SET threads=1 only): ~15 980 req/s aggregate (16 threads × 20k iters) → **4.8×** of 1-thread ~3 338 req/s.
   - enable_logging=false (and other logging variants): +0.9% to +3% at best on matched seeds.
   - preserve_insertion_order=false + progress_bar + profiling off: worse.
   - All combined: ~16 399 req/s (~+2.6%).
   - **Gate (≥20% required):** max reliable win ~3%. **GATE FAILED.** No server change.

   Bonus isolation run (same micro, same workload):

   ```
   16 threads, shared DB instance:  ~15 980 req/s  (4.8× serial)
   16 threads, 16 SEPARATE :memory: instances: ~20 275 req/s  (+27% over shared, 6.1× serial)
   ```

   Separate instances still only 6.1× of one thread, not 16×.

**Hardware correction (owner machine, sysctl-verified):** `hw.ncpu=16` on Mac16,5 is 12 performance cores + 4 efficiency cores. A CPU-bound workload's realistic linear ceiling is ~13×, **not** 16×. Framing against that:

- shared instance: ~4.8× (~37% of realistic ceiling)
- separate instances: ~6.1× (~47% of realistic ceiling)

The wall is real, and the numbers are now sized honestly.

**Mechanism.**

DuckDB is an OLAP execution engine. For each query it constructs a fresh `ThreadContext`, an `OperatorProfiler`, per-pipeline state, metrics scopes, and (for logging) calls `LogManager::CreateLogger` which takes a `std::mutex`. Those costs are sized for queries that run milliseconds to seconds and that amortize their setup over thousands of rows or complex plans. At the /search shape (~0.28–0.30 ms per query from the 3.3–3.5k serial floor), the fixed per-query setup **dominates** the actual scan + json_group_array work.

When 16 connections simultaneously hammer the same `DatabaseInstance`, the shared components (LogManager singleton, allocator arenas, any global executor tables) are contended on every tiny task. The per-connection `threads=1` setting (brain.cpp:460) prevents intra-query morsel thrash, but does not remove the cross-connection cost of constructing and tearing down executor state 16× as often. The B5 separate-instances run proves the tax is partly instance-global; the fact that even 16 isolated instances only reach 6.1× proves the per-task construction itself is expensive for this query size. This is a design-assumption mismatch (OLAP per-query model vs. high-churn micro-OLTP), not a configuration bug or a quackapi C-layer problem.

**Verdict: DEFEATED by measurement — and not fixable by configuration.**

The hypothesis does not hold for this workload shape against a shared DuckDB instance. The 16-worker server is doing real work on 6–11 cores, delivers stable 0-failure throughput, and is still within ~2.7k of the no-database opponent. But linear scaling with workers is not observed, settings knobs do not recover it, and the mechanism is inside DuckDB's per-task setup.

**The philosophical close (the price of actually being a database).**

The single cell quackapi loses is the one where the opponent does **zero database work** per request: it serves a Python dict. Every other cell (including every /search cell against the real-DuckDB FastAPI variant, and all lighter paths) is a win, often by 1.5–4×, with 0 failures where the Python+duckdb path produces Length/Non-2xx errors.

Matching the 21.8k pure-mem number while still doing real SQL per request would require exactly what Round 3 already rejected: a materialized response cache that turns the handler into a lookup instead of executing the query. That would be the materialization mirage again — impressive numbers that stop being "a database."

The honest position is therefore:

- quackapi wins every like-for-like cell (framework cost + actual DB work per request).
- The remaining gap on /search c64 vs 16-worker in-memory is the observable price of actually executing a query instead of returning a dict.
- We document that price precisely (4.8× observed vs ~13× realistic ceiling on this hardware; the exact frames; the B5 gates) rather than faking past it.

**The two-gates meta-lesson.**

B4 (prepared statements) and B5 (settings) both executed their mandatory Phase-1 microbench first, hit their explicit numeric gates (<15% and <20%), and correctly stopped before any server edit or re-bench. Both would have shipped noise (a few-percent "win" that would have been lost in variance or would have regressed something else). Measure-gate before every perf build is now project law.

**What could actually move the needle (untested directions, not plans).**

- Upstream DuckDB: cheaper per-task ThreadContext/OperatorProfiler construction, or a lock-free / per-connection logger path that does not take the std::mutex on every pipeline task for tiny queries.
- Change the workload shape: batch N logical requests into one DuckDB query (amortizes setup). This changes handler semantics and client-visible latency.
- Accept the niche: the current design already wins on stability and on every path that actually touches the database. For pure in-memory dict workloads the right tool is not a database at all.

These are observations from the evidence, not commitments. The wall is documented; the ledger records the cost of doing real work.

**DRAFT — pending merge into edges.md.**
