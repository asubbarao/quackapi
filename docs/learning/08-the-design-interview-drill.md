# 08 — The Design Interview Drill

This is not a recap. It is a pressure chamber.

You can recite what quackapi is. The gap is constructing answers live when an interviewer asks "how would you...", "why didn't you...", "what breaks when...", or "convince me your numbers aren't noise." The repo already contains the complete worked example of the optimization loop, the honest edges, the verification regime, and the trade-offs. This drill turns those artifacts into question-shaped ammunition.

Rules for use:
- Open the cited files. Do not memorize scripts.
- Answer out loud in 60–90s skeletons using the 3–5 beats and the numbers/mechanisms.
- When the honest answer includes a limit ("my system can't..."), say it first. The ledger is the proof of judgment.
- Every claim below is pulled verbatim from the listed artifacts. If it is not in edges.md, edges_round5/6, the learning 00–07, FEATURE_GAP_MATRIX, BENCH_HEADTOHEAD, the B*/PROFILE/R2_RESULT docs, AGENT_DRIVEN_DEVELOPMENT, or COMMUNITY_EXT_PATH, it is not here.

Format of each entry:
**Q:** (interviewer phrasing)
**What they're really testing:**
**The answer you should be able to construct** (3–5 beats + file cites)
**Follow-up trap:** (gotcha + one-line through)

---

## 1. Framework decomposition

**Q:** Design a web framework.

**What they're really testing:** Can you decompose into listener / router / validator / executor / serializer layers and name the oracle-vs-hot-path split instead of reciting "FastAPI inside DuckDB."

**The answer you should be able to construct:**
- Listener + accept loop + 16-worker pthread pool lives in C (quackapi_brain.cpp); each worker holds one persistent Connection + SET threads=1; work is pulled from a bounded queue under condvar (edges.md:1b, docs/learning/05:20-60).
- Router + validator + static serializer + handler-sql templating moved into compiled C++ structs at boot (g_rt RouteTable with pre-split segments + literal_count + ParamDef); DuckDB only runs the final rendered handler point query (ext-cpp/B2_RESULT.md:10, edges.md:440-460).
- Executor is the DuckDB query on the rendered hsql (or zero-DB for statics / 404 / 422).
- Oracle is the pure-SQL `handle_request` CTE pipeline in framework.sql (Tier-1); the C path is the compiled mirror required to cross the ~1k floor (docs/learning/05, B2_RESULT parity harness, R2_RESULT 26/26).
- The split is deliberate: registry tables are the single source of truth; C owns the per-request hot path; statics and OpenAPI pre-render are served without touching the DB.

**Follow-up trap:** "But then it's not really 'inside the database' anymore." The tables (routes, param_schema) and the handler SQL remain SQL; the C layer only does the match/validate/templating that the OLAP macro could not do at speed. DuckDB still executes the application logic.

---

## 2. Request lifecycle

**Q:** Walk me through exactly what happens when a request hits your server.

**What they're really testing:** Concrete mechanism trace with phase and file anchors, not "it goes through the router."

**The answer you should be able to construct:**
- Socket: serve_brain opens listen socket (AF_INET, SOCK_STREAM), SIGPIPE=IGN, TCP_NODELAY on accepted conns (brain.cpp:500-520).
- Accept loop (one thread) does accept, enqueues fd to g_q under g_qm mutex + signals cond (brain:489).
- Worker (16 detached pthreads) dequeues, calls handle_conn_on (brain:450-480): read into fixed 64k buffer, manual parse of request line + headers + body.
- Route: if g_rt, quack_route does split, literal-count match + most-literal tiebreak, param extraction, try_cast-equivalent validation, constraint checks, handler_sql templating (brain:920-1100; exact mirror of framework.sql QUALIFY + list_* + try_cast).
- Static fast paths (/ping, /q1, /q2, /q3, health/docs/openapi) short-circuit before DB (brain:215).
- Execute: for dynamic, g_ddb_query on the rendered handler_sql using the worker's persistent conn; first column value becomes body (or chunked for stream).
- Respond: write status + headers + body (Content-Length or chunked for SSE), close or keep depending on client (brain:370-400).
- All of the above is anchored in brain.cpp + the registry load path in quack_load_registry.

**Follow-up trap:** "What about keep-alive?" Ruled out as non-bottleneck: /ping does 38k req/s with connection-close; ab -k added nothing on loopback (edges.md:290).

---

## 3. The crossing from 1k to 30k

**Q:** How did you make it fast?

**What they're really testing:** You can name the concrete work that left the per-request path and where the evidence is, not "we rewrote it in C."

**The answer you should be able to construct:**
- Pure-SQL floor: ~1k req/s flat (edges.md:410 after Round 3 rip-out); every request paid for the full 13-CTE pipeline (QUALIFY row_number on literal_count, list_zip/filter/transform/reduce, map_from_entries, json_group_array/object) re-bound and re-planned each time.
- The 1k→~19–41k crossing (B2): registry (routes + param_schema) loaded once into read-only g_rt structs at boot / reload; per-request work became C segment match + extraction + validation + templating; statics (health, docs, openapi, 404) became zero-DB; DuckDB only ever ran the final handler point query (ext-cpp/B2_RESULT.md:40, edges.md:440).
- Oracle remained: B2/B3/R2 parity harnesses compared C decision vs `SELECT * FROM handle_request(...)` (16/16 then 26/26 byte or semantic) (B2_RESULT, R2_RESULT).
- Registry-to-structs + static pre-render is the mechanism; nothing was materialized into response tables (the mirage that hit 28k on file DBs was rejected).

**Follow-up trap:** "So why not just precompute the whole responses?" That would be the materialization mirage again — a router is not a cache of prior answers; it must decide on the live registry for every request (edges.md:380-400).

---

## 4. Finding the bottleneck

**Q:** How would you find the bottleneck in a system like this?

**What they're really testing:** The B4 → profile → B5 loop as the actual practiced answer, not generic "use a profiler."

**The answer you should be able to construct:**
- Hypothesis first: "parse/bind/plan tax on the handler SQL is the remaining cost after B2."
- Gate before code: B4 standalone micro (exact /search shape, 6 distinct literal texts cycled, full result extract) showed prepared only +2.5–4.6% vs string; gate was <15% → STOP, no server edit (ext-cpp/B4_RESULT.md:20-50).
- When hypothesis dies under load: PROFILE_SEARCH on live 16-worker server under sustained /search c64: workers busy inside duckdb_query → Executor → ThreadContext ctor, LogManager::CreateLogger + mutex, OperatorProfiler + alloc; C layer and queue near zero attribution (PROFILE_SEARCH.md:30-50).
- Next gate: B5 settings micro (enable_logging, profiling off, preserve_insertion_order etc.) on identical 16-thread workload: max reliable +~3%; gate ≥20% → STOP (B5_RESULT.md:50-80).
- The discipline is: micro-gate before edit; profile live under the real load when the gate passes or the server regresses; separate the "our layer" hypothesis from the engine cost.

**Follow-up trap:** "Did you try X setting?" The micro already did, under the exact workload shape, before any claim was made.

---

## 5. Scaling wall

**Q:** Your service isn't scaling linearly with threads — debug it.

**What they're really testing:** You can rule out your own layer with evidence, isolate shared vs instance cost, and size the honest ceiling against hardware.

**The answer you should be able to construct:**
- Serial floor on /search-shaped work: ~3.3–3.5k req/s per connection (B4/B5 micros).
- 16 workers on shared DatabaseInstance: ~15.9–17.1k aggregate (4.8–4.9×) under c64 (BENCH_HEADTOHEAD.md:60, PROFILE_SEARCH, B5 cell 1).
- Rule out own layer: profile stacks showed workers inside DuckDB exec, not on g_q cond or C path; dual-ab summed to same ceiling (ruled out client); top showed 550–1100% CPU (real multi-core work) (PROFILE_SEARCH.md:40-60).
- Shared vs separate isolation: 16 threads on one DB 15.98k vs 16 separate :memory: instances 20.3k (+27%); even separate only 6.1× serial (B5_RESULT.md:60).
- Hardware ceiling: hw.ncpu=16 is 12P + 4E; realistic ~13× for CPU-bound; observed shared ~37% of that ceiling (edges_round6_draft.md:60-70, docs/learning/07:20).
- Root: OLAP engine per-task construction (ThreadContext, OperatorProfiler, LogManager mutex, allocator churn) sized for ms–s queries, not 300µs point queries under 16-way churn (07, PROFILE).

**Follow-up trap:** "So add more workers?" The profile already showed the C accept side is not saturated; adding workers would just increase contention on the same per-task costs.

---

## 6. When not to use the system

**Q:** When would you NOT use your own system?

**What they're really testing:** The edges ledger as the answer format — precise, cited defeats and real limits, not "it depends."

**The answer you should be able to construct:**
- Async upstream I/O or high-concurrency blocking-slow work: model is 16 blocking pthread workers with persistent conns; no async runtime; trilemma forces C to own the accept loop for browser-facing concurrency (edges.md:1c, #8, FEATURE_GAP_MATRIX:80).
- Pure in-memory dict workloads: the one cell quackapi loses at c64 is exactly the case where the opponent does zero DB work (fastapi_mem 21.8k vs quackapi 17.1k on /search); every other cell (real SQL) is a win or tie with 0 failures (BENCH_HEADTOHEAD.md:60, edges_round6:80).
- Multipart uploads >~64 KB or true streaming to disk: C reader is single fixed 64k read with no boundary parse in shipped path (edges.md:203-220).
- TLS termination: zero SSL anywhere in socket/accept path (FEATURE_GAP, COMMUNITY).
- Yield-style open-txn DI across handler: one-shot dispatch model has no cross-statement resource lifetime or guaranteed finally (edges.md:5-6).

**Follow-up trap:** "But you could add X." The ledger records where the current abstraction tears; adding the missing piece changes the model (e.g. async workers) and must be costed.

---

## 7. Correctness regime

**Q:** How do you know it's correct?

**What they're really testing:** Oracle + byte-parity harness + adversarial re-verification culture, including the B3 fiction catch.

**The answer you should be able to construct:**
- Oracle-first: pure-SQL `handle_request` macro defines the contract (status, content_type, body, handler_sql); Tier-1 tests and parity harnesses run against it without any HTTP server or C extension loaded (test/tier1_handle_request.test.sql, parity_b2.sh).
- Byte parity gates: B2 16/16, B3_VERIFY 16/16 after fixes, R2 26/26 including new header/cookie/form/redirect cases (B2_RESULT, B3_VERIFY_RESULT, R2_RESULT).
- Adversarial verification: B3 initial "all tests passed" claim was fiction (deadlock 100%); independent re-run + harness caught it because the rule is "tested means you or a second adversarial agent actually ran it" (AGENT_DRIVEN_DEVELOPMENT.md:40, B3_VERIFY).
- Tier-1 without server: routing, validation, 422 shape, templating, and even CREATE ROUTE side effects can be asserted via `quack_route_decision` and `quack_apply_route` calls in pure SQL (R2, B3_VERIFY).

**Follow-up trap:** "But the C path could still drift." The parity harness is the gate before any perf claim or B3 syntax work; it is re-run on every round.

---

## 8. Concurrency model and locks

**Q:** What's your threading model and where are the locks?

**What they're really testing:** 16 workers + per-conn ClientContext + the non-recursive mutex war story as concrete evidence.

**The answer you should be able to construct:**
- Model: one accept thread + 16 detached worker pthreads; each worker opens one persistent Connection at startup and reuses it; queue g_q (bounded) + condvar for handoff; no locks held across the DuckDB call in the hot path after dequeue (docs/learning/05:40, brain.cpp:450).
- Per-worker: SET threads=1 so individual queries do not request the full morsel pool (05, 07).
- The lock that bit: ClientContext mutex is non-recursive; Binder::Bind (and table-func execute) already holds it; calling context.Query() on the same context deadlocks (edges_round5_draft.md:30-40).
- The B3 war story: initial RouteDdlPlan called ClientContext::Query while inside Binder::Bind → __psynch_mutexwait forever; also table-func never emitted cardinality 0 (infinite loop). Fix: plan only packages; side effects on fresh Connection(con from DatabaseInstance::GetDatabase(context)) at execute time; GlobalState done flag + SetCardinality(0) (edges_round5, learning/03:50-70).
- Cross-connection: 16 ClientContexts against one DatabaseInstance; shared singletons (LogManager, allocator) are the scaling tax, not the per-worker mutexes (07, PROFILE).

**Follow-up trap:** "Could two workers deadlock each other?" No — each has its own ClientContext and lock; the deadlock was always self-deadlock on the same context from inside a callback.

---

## 9. DDL transactional semantics

**Q:** What are your DDL's transactional semantics?

**What they're really testing:** Honest known limitation + mechanical why, not "it's fine."

**The answer you should be able to construct:**
- CREATE ROUTE / DROP ROUTE are real ParserExtension DDL; they write routes + param_schema then call quack_reload_router (edges_round5_draft.md:60).
- Side effects run on a fresh Connection created from DatabaseInstance::GetDatabase(context) inside the table function execute callback (03:60, edges_round5:70).
- Consequence: the INSERT/DELETE/CHECKPOINT + reload commit in a separate transaction from the statement that issued the CREATE ROUTE. A CREATE inside an outer txn that ROLLBACKs will still persist the route and the g_rt update (edges_round5:100, learning/03:80).
- This is permanent for the fresh-Connection technique; documented as a real semantic edge.
- Cross-process: g_rt is per-process static; other processes see the row only after their own LOAD + reload (edges_round5:100).

**Follow-up trap:** "Why didn't you fix the transaction split?" The split is the mechanical price of running side-effect SQL from inside a bind/execute callback without re-entering the held mutex; changing it would require internal engine changes or a different registration model.

---

## 10. Caching vs materialization

**Q:** Why not just cache responses?

**What they're really testing:** You understand what a router *is* and why the 28k number was a lie.

**The answer you should be able to construct:**
- Round 3 experiment: precomputed exact routes + response bodies into route_exact + response_cache tables; /health hit 28k via hash probe on file DB (edges.md:380).
- That was the materialization mirage: five derived tables persisted as relations; the "router" was a cache lookup, not a decision procedure; it violated the project's own "no materialized derived state" rule and measured disk cache speed, not routing.
- After rip-out: honest ~1k floor, flat; the router *is* the self-contained SQL query (or its C mirror) over the live registry every time (edges.md:400-420).
- FastAPI does not cache prior responses into tables for its router; it walks an in-memory structure (or compiled tables) and executes the handler. The 28k proved the wrong thing.

**Follow-up trap:** "But for static routes it would be fine." Statics are already zero-DB in the shipped C path; the mirage was precomputing the dynamic decisions and bodies.

---

## 11. Validation and errors

**Q:** How do you do validation and errors?

**What they're really testing:** Schema-as-data + exact 422 shape + constraint tables, not "we have try_cast."

**The answer you should be able to construct:**
- Registry is data: routes + param_schema (name, type, required, location, constraint_json) are the SSOT; CREATE ROUTE and register_* populate them (framework.sql, app.sql, B3_VERIFY).
- Validation in both oracle and C: missing required, try_cast int/float/bool (exact error codes: int_parsing etc.), le/ge from constraint_json producing "less than or equal to N" (brain:1010, framework:210-230).
- 422 shape: identical `{"detail":[{"type":"...","loc":["path","q"],"msg":"..."}]}` built by json_group_array + json_object in SQL and by hand in C (framework:240-260, brain:1020-1050, B2 parity cases).
- Errors are first-class routes in the matrix; 422 cases are part of the byte-parity gate.

**Follow-up trap:** "What about nested models or custom validators?" Flat only (HARD gap); custom via SQL CASE or handler logic or pre-middleware (FEATURE_GAP_MATRIX:60, NATURAL).

---

## 12. Extensibility limits

**Q:** How would you add WebSockets / TLS / uploads?

**What they're really testing:** You can read the FEATURE_GAP build-order list and name the concrete mechanisms that resist, not vibes.

**The answer you should be able to construct:**
- WS transport defeated in isolation (serve_ws.sql does RFC6455 upgrade + frame loop in ducktinycc C); integration HARD because main brain is one-shot HTTP, no shared registry/DI/auth surface yet (edges.md:118, FEATURE_GAP:120).
- TLS: zero SSL in socket/accept code (plain AF_INET/SOCK_STREAM only); disqualifying without external proxy (FEATURE_GAP:1, COMMUNITY).
- Multipart streaming: C reader is single `read(fd, req, 65535)` into fixed stack buffer; no Content-Length loop, no boundary parser in shipped handle_conn_on; ducktinycc allocation constraints also block (edges.md:203-224). Small bodies (<64k) reach handler; larger truncate. Ranked #2 in build order.
- The resistance is mechanical (fixed buffer + no async/stream sink + separate serve path), not "we didn't get to it."

**Follow-up trap:** "You could just proxy." Then you have a proxy, not a self-contained framework; the claim was in-process.

---

## 13. Shipping and distribution

**Q:** How would you ship and distribute this?

**What they're really testing:** Community extension path mechanics + maintenance cost of internal APIs.

**The answer you should be able to construct:**
- Path exists: description.yml PR to community-extensions; CI via extension-ci-tools matrix; DuckDB team signs; users do INSTALL ... FROM community; LOAD (COMMUNITY_EXT_PATH:10-20).
- Reality: POSIX sockets/pthreads/dlsym confined to brain.cpp; routing + ParserExtension + CREATE ROUTE are C++ API only and portable. Can use excluded_platforms for windows/wasm (httpserver precedent) (COMMUNITY:50-60).
- Costs: test/sql must be real sqllogictest (current is waddle placeholder); .github/workflows must reference correct name; every DuckDB release triggers mass rebuild; ParserExtension + custom dlsym symbol resolution are the top maintenance surface flagged in the study (COMMUNITY:80-100).
- No vcpkg deps beyond template; MIT; but Windows absent and template cleanup incomplete.

**Follow-up trap:** "Why not just ship the pure-SQL version?" The pure track tops at ~1k and cannot do CREATE ROUTE syntax; the C++ track is what makes the DDL and the perf crossing possible.

---

## 14. Benchmark credibility

**Q:** Your benchmark says you beat uvicorn — convince me.

**What they're really testing:** Methodology details that survive "cherry-picked" or "client-limited" attacks.

**The answer you should be able to construct:**
- Same box, same ab (ApacheBench 2.3), same flags (-n 8000 -c 8/64 -k), same payloads, sequential one server at a time, only lsof-derived PIDs, warmup pass, parity curl before matrix (BENCH_HEADTOHEAD.md:10-30).
- Opponent includes the in-memory ceiling variant (fastapi_mem, no DB work) so the loss cell is visible; duckdb-backed FastAPI produces real Length/Non-2xx failures under load while quackapi has 0 (matrix row D/E vs A).
- Failed requests reported for every cell; quackapi 0 across the board.
- Client-limit check: dual concurrent ab (c32+c32) on /search summed to same ~17k as single c64 ab (PROFILE/BENCH); /health single c64 ~31-42k proves ab can push higher.
- Variance honesty: raw "Requests per second:" lines pasted; /search is the only loss and only vs pure-mem at high c; all other like-for-like (real DB work) are wins or ties.
- Numbers are from the actual ab output, not summaries.

**Follow-up trap:** "What about variance across runs?" Raw repeated runs (multiple cells, B2/B5/PROFILE) show the same 4.8× scaling and the same ceiling; the gate discipline prevents shipping noise.

---

## 15. Ownership without writing the C

**Q:** You said you didn't write the C++ — defend your ownership.

**What they're really testing:** Specification, verification, reversal decisions, and war stories as evidence of depth (not "I prompted").

**The answer you should be able to construct:**
- Thesis and decomposition were owner: FastAPI-killer for full-DuckDB stacks, oracle-first (SQL brain leads, C mirror follows), measure-gate before every perf build, "no materialized derived state" rule (AGENT_DRIVEN_DEVELOPMENT.md:20).
- Reversal that mattered: killed the 28k materialization (tables as router) in one sentence because it measured a cache, not routing; that decision became the project's load-bearing idea (AGENT:20).
- Override that mattered: chose full C++ API (ParserExtension for real DDL) over three agents' C-ABI recommendation because the strategic property (CREATE ROUTE as first-class syntax) was worth the build and maintenance cost (AGENT:30).
- Verification culture as owner rule: "tested means deployed and observed, never relay an agent's claim"; caught the B3 deadlock fiction that "all tests passed" (AGENT:40).
- The war stories are the proof: bind-time deadlock (phase/lock mechanics), materialization mirage (what a router is), scaling wall (OLAP per-task tax under micro-OLTP, honest measurement), B4/B5 gates (discipline that stops noise).
- Scope discipline: when the transaction edge surfaced, owner said "document it, move on; work goes into making this performant."

**Follow-up trap:** "So the agents did the work." The agents executed; the owner repeatedly answered "what do we actually want?", designed the evaluation (byte parity, in-memory ceiling opponent, measure-gates), allocated the scarce resource (judgment + tokens), and refused to ship things he could not explain.

---

## 16. Backpressure and queue design

**Q:** How does your server handle overload or slow clients?

**What they're really testing:** Concrete queue mechanics and the honest "no sophisticated backpressure" admission.

**The answer you should be able to construct:**
- Bounded queue: g_q[4096] (or equivalent) in the accept-to-worker handoff; workers dequeue under mutex/cond (edges_round6_draft, learning/05, FEATURE_GAP:80).
- On full: close the new fd (simple backpressure by dropping the connection).
- No token bucket, no per-client rate, no drain on shutdown; workers are detached; SIGPIPE only is handled.
- This is sufficient for the CPU-bound JSON case that wins the matrix; for I/O-heavy or adversarial load it is exactly the "HARD" gap listed.

**Follow-up trap:** "What happens to the accepted connection when the queue is full?" It is closed immediately in the accept path; no work is enqueued.

---

## 17. The pure-SQL floor and query shape cost

**Q:** After all the C work, why is the dynamic path still not at the /q2 34k number?

**What they're really testing:** You can size the remaining gap to the actual per-operator tax instead of hand-waving "C is faster."

**The answer you should be able to construct:**
- /q2 (point query on users, no brain): 34k+ and scales to c=64 (edges.md:340).
- After macroless lean + MATERIALIZED: dynamic /users/1 ~1.7k (still ~20× short); /health static pre-render ~1.9k in pure SQL era.
- Diagnosis in Round 3: the gap is the single pass through the 13-CTE brain pipeline itself (~580µs vs ~30µs); QUALIFY window, multiple list lambdas, two map_from_entries, json construction — ~30 operators, each with fixed setup DuckDB amortizes over large cardinality but a 1-row request pays in full (edges.md:360-370).
- The C router removes the *routing* part of that tax; it does not remove the handler's own json_group_array etc when the handler is non-trivial (/search loses the most).

**Follow-up trap:** "So the brain is still the problem." For routes that only execute a point handler, C routing + statics get us to 25–41k. The remaining gap on heavy handlers is the shape of the handler query, not the framework.

---

## 18. Capacity planning on real hardware

**Q:** How do you capacity-plan this on P/E-core machines?

**What they're really testing:** You use the measured ceilings and hardware facts instead of "16 workers = 16×."

**The answer you should be able to construct:**
- Serial micro on the pillar workload: ~3.3k req/s (B4/B5).
- Realistic linear on this Mac (12P+4E): ~13×, not 16× (edges_round6_draft:60).
- Observed shared-instance: 4.8× (~37% efficiency); separate instances 6.1× (~47%).
- Therefore: for CPU-light handlers the 16-worker server is already in the 17–30k band on real DB work and wins the matrix; for pure-mem dict workloads the right tool is not this system; for I/O-heavy the blocking model is the limit.
- Planning knob that exists: separate :memory: instances per worker (or per process) recovers some but not all of the tax.

**Follow-up trap:** "Just buy a big machine." The per-task construction tax scales with query rate, not core count; more cores without cheaper ThreadContext construction still pays the fixed cost per micro-query.

---

Use this drill with the source and the result docs open. The goal is not to sound impressive; it is to be able to navigate the hard follow-ups with the actual numbers and the actual scars from the repo.

When an interviewer says "how would you optimize it?", the answer that survives is the one you watched happen four times: gate first, profile live when it dies, document the wall, keep the ledger.
