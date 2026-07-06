# 07 — Parallel scaling inside a database — why 16 workers gave 5x

The naive model is "serial rate × workers = linear scaling." On a 16-core machine a workload that does 3.3k req/s on one connection "should" deliver ~50k with 16 workers. The server ships with exactly that model wired in: 16 detached pthreads, each with its own persistent connection, each forced to `SET threads=1` (brain.cpp:460), each pulling work from a condvar queue.

Measurement defeated the model. 16 workers on a shared instance deliver ~4.8–4.9× of serial, not 16×. The gap is not in the C layer, not in queue contention, and not fixed by any DuckDB setting. This document explains where the naive model breaks, how the evidence located the cost inside DuckDB, and the discipline required to keep future claims honest.

## The serial floor and the observed scaling

From controlled microbench (B4_RESULT.md and B5_RESULT.md, identical /search-shaped work cycling 6 distinct literal texts):

- 1 connection, 1 thread: 3 338–3 532 req/s (0.28–0.30 ms/query).
- 16 threads, shared `DatabaseInstance`: 15 980 req/s aggregate → **4.8×** serial (B5 cell 1).
- Server under load (PROFILE_SEARCH.md): ~16.5–17.1k on /search c64 with 16 workers (BENCH_HEADTOHEAD.md:17128.31).

Even the best-case isolation experiment (B5 cell 6) only reached:

- 16 separate `:memory:` instances: 20 275 req/s → **6.1×** serial, still far from 16×.

## Where the naive model breaks

The model assumes that the per-query work is purely "useful" CPU that can be multiplied by adding threads or connections. Four costs violate that assumption for tiny OLTP-ish queries:

1. **Fixed per-query executor setup.** Every query, even a 3-row starts_with + json_group_array, pays for a fresh `ThreadContext`, `OperatorProfiler`, pipeline state, and metrics. These objects are constructed on the hot path inside `PipelineExecutor::PipelineExecutor` (PROFILE_SEARCH.md stacks).

2. **Shared global structures under churn.** `LogManager::CreateLogger` acquires a `std::mutex` on every pipeline task that needs logging. Allocator arenas and any global executor tables are also shared across all connections to the same `DatabaseInstance`. At hundreds of microseconds per query the fixed setup and occasional lock acquisition dominate; the actual scan work does not.

3. **Allocator traffic.** 10 s of /search load produced ~21 609 `nanov2|malloc|operator new` samples vs ~3 370 for the lighter /users/1 handler on the same box (PROFILE_SEARCH.md:150). The per-task ctors plus expression temporaries + JSON construction create high small-allocation churn; when 16 connections do it simultaneously the allocator paths contend (some `_os_unfair_lock` and `__ulock` inside the malloc paths).

4. **Hardware reality.** `hw.ncpu=16` (BENCH_HEADTOHEAD.md) on Mac16,5 is 12 performance + 4 efficiency cores. A CPU-bound workload's realistic ceiling is ~13×, not 16×. Against that honest ceiling the shared instance achieved ~37% efficiency; separate instances ~47%.

The `SET threads=1` per worker (brain.cpp:456–460) is the correct countermeasure for the opposite problem (16 queries each asking for 16 morsels would thrash the scheduler). It does not eliminate cross-connection setup cost.

## How to read a `sample` profile to find the cost

PROFILE_SEARCH.md is the canonical example. Key reading rules that turned raw stacks into a diagnosis:

- **Idle vs load thread states.** Idle: schedulers on `semaphore_wait_trap`; main thread in `block_forever`. Load: 16 workers predominantly **running inside duckdb exec**, not parked on the C queue. That single observation ruled out "our pthread pool or g_q is the limiter."

- **Frame aggregation, not single lines.** The top worker thread showed 5 567 / 5 983 samples inside `handle_conn_on → duckdb_query → ... → Executor`. Inside that: `ThreadContext::ThreadContext`, `LogManager::CreateLogger + std::mutex::lock`, `OperatorProfiler::OperatorProfiler + operator new`. The C socket/read/write and the quack_route match loop were near-zero outside the query call.

- **Contrast runs.** Same 10 s sample window on /users/1 (higher throughput) produced ~6× fewer alloc samples and ~2× the req/s. The difference is the handler shape, not the server.

- **Cross-checks.** Dual ab summing to the same ceiling as single ab ruled out client saturation. `top` showing 550–1 100% CPU ruled out "only a few cores are busy." Queue wait samples (tens) ruled out g_qm contention.

When a profile shows workers busy in the engine rather than blocked in your code, the next question is "what does the engine construct per statement?"

## The isolation technique: shared vs separate instances

B5's bonus cell is the cleanest demonstration that the tax is partly shared state:

- Same compiled micro, same workload text, same NTHREADS=16, only the `DatabaseInstance` changes.
- Shared file-backed DB: 15 980 req/s.
- 16 independent `:memory:` (own open + seed + conn per thread): 20 275 req/s (+27%).

The delta proves some contention lives at the `DatabaseInstance` (LogManager, global allocator arenas, etc.). The fact that even the isolated case is still only 6.1× proves the dominant cost is the per-query construction itself, which is paid on every connection regardless of sharing.

This technique (vary only the instance while holding everything else fixed) is the way to distinguish "our C layer serializes" from "DuckDB's per-task model is expensive under concurrent conns."

## Measure-gate discipline (B4 and B5 as case studies)

Before B4 or B5 touched the server, both ran a mandatory standalone microbench with an explicit numeric gate and a "STOP if not met" rule:

- B4: prepared statements had to be ≥15% faster on the exact /search shape with distinct SQL text per iter. Result: 2.5–4.6%. Gate failed, no code change, no re-bench.
- B5: any setting combo had to deliver ≥20% over the matched 16-thread baseline. Max reliable: ~3%. Gate failed, no code change.

Both gates executed **before** any edit to `quackapi_brain.cpp` or any new server run on a 184xx port. The outcome is a success of the discipline, not a failure of the idea: two builds that would have shipped noise were prevented.

The rule is now project law: for any claimed perf change, write the gate first (standalone, reproducible, raw numbers), run it, and only proceed if the gate passes. The gate lives in the result doc; the doc is written before the code that would be measured.

## Cross-links

- 03 (locks/connections): the per-worker `g_ddb_connect` + `SET threads=1` creates 16 distinct `ClientContext`s against one `DatabaseInstance`. The mutex story in 03 is about re-entrancy from inside the binder; this edge is about cross-connection contention on shared engine singletons during normal top-level queries.
- 05 (the server tour): `worker_main` (brain.cpp:450), the persistent conn per worker, the `SET threads=1` comment, and `handle_conn_on` (brain.cpp:208) are exactly the 16-connection model that exposed the scaling limit. The profile showed the workers were spending their time in the path 05 documents.

## Read it yourself (guided order)

1. `bench/BENCH_HEADTOHEAD.md:60` — the single loss cell (/search c64) and the table that shows quackapi wins everywhere else.
2. `ext-cpp/B4_RESULT.md:20` — the microbench setup (distinct SQL texts cycled) and the raw 3471 / 3637 numbers that became the serial floor.
3. `ext-cpp/B4_RESULT.md:70` — "Why the win was small" paragraph that reframes the problem away from parse/plan.
4. `ext-cpp/PROFILE_SEARCH.md:90` — the key worker stack under /search load; locate `ThreadContext`, `LogManager::CreateLogger`, and the mutex frames.
5. `ext-cpp/PROFILE_SEARCH.md:140` — allocator and lock counts (21k vs 3.4k contrast).
6. `ext-cpp/PROFILE_SEARCH.md:160` — idle vs load thread-state diagnosis and the dual-ab cross-check.
7. `ext-cpp/B5_RESULT.md:40` — the 1-thread baseline cell and the 16-thread shared baseline (15 980 req/s).
8. `ext-cpp/B5_RESULT.md:100` — the separate-instances cell and the +27% observation.
9. `ext-cpp/B5_RESULT.md:130` — the gate decision and the explicit statement that the limit is inside DuckDB internals.
10. `ext-cpp/src/quackapi_brain.cpp:450` — `worker_main` and the `SET threads=1` block (the comment explains the scheduler-thrash problem it solves).
11. `ext-cpp/src/quackapi_brain.cpp:208` — entry to `handle_conn_on` (the path the profile proved was not the limiter).

## Comprehension questions

1. A colleague says "just add more workers or set threads higher." Using only the numbers in B5 cells 1 and 6 plus the hardware correction, explain in two sentences why that cannot reach the 13× ceiling for this query shape.

2. In the PROFILE_SEARCH stacks, the C accept/read/route/write path contributed almost no samples while the workers were under load. What single observation from the thread headers and idle baseline proves the workers were not blocked waiting for work from the C layer?

3. B5 ran the identical micro on a shared DB vs 16 separate instances and saw +27%. What does the remaining gap (6.1× vs 16×) tell you about whether the dominant cost is instance-global locks or per-query construction work?

4. Both B4 and B5 wrote their result documents (including the gate rule and the STOP decision) before editing any .cpp. Why does writing the gate in the doc before the code change matter more than the numeric result itself?
