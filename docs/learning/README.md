# quackapi Learning Track

Teaching documents for an elite SQL engineer who wants to *read* every mechanism in the C++ extension, understand why things deadlock or go fast, and be able to explain the engine internals without hand-waving.

The track is deliberately narrow: only the parts of C++ and DuckDB extension machinery that are actually used in this repo. Every claim is anchored to real lines in `ext-cpp/src/quackapi_extension.cpp`, `ext-cpp/src/quackapi_brain.cpp`, `framework.sql`, and the B3 war story in `edges_round5_draft.md`.

## Reading order

Read in order. Each document assumes you have internalized the previous ones.

1. `00-reading-cpp-for-a-sql-engineer.md` — the minimum C++ you need to navigate the two source files (headers, namespaces, `&` vs `*`, `unique_ptr`+move, `extern "C"`, lambdas, manual string work). Every example is a direct quote from the repo.
2. `01-what-a-duckdb-extension-actually-is.md` — dynamic library, `DUCKDB_CPP_EXTENSION_ENTRY`, `LOAD`, ABI pinning to the exact DuckDB submodule version, why the C++ API track was required for `ParserExtension`, relation to the tcc-JIT track.
3. `02-the-life-of-a-query.md` — parse / bind / plan / execute phases in DuckDB. Where `ParserExtension` hooks fire, where table function bind/init/execute fire. The two B3 phase bugs explained as phase violations.
4. `03-locks-connections-transactions.md` — `DatabaseInstance` vs `Connection` vs `ClientContext`. The non-recursive mutex. Exact reason `ClientContext::Query()` from inside `Binder::Bind` self-deadlocks. The fresh-Connection fix and its permanent transaction-split consequence.
5. `04-the-table-function-protocol.md` — bind (schema), init (state), execute called repeatedly until a 0-cardinality chunk. Why the missing done flag caused infinite emission. Contrast with the SQL table macros you already know.
6. `05-a-web-server-inside-a-database.md` — full tour of the brain: socket/accept, pthread pool of 16, `g_rt` static, line-by-line port of the `handle_request` CTE logic into C, static pre-rendering, what actually left the hot path for the 15-30x win.
7. `06-the-build-system.md` — what `make release` does, why first build is slow, how to read a 60-line template error, incremental rebuilds after touching one .cpp.
8. `07-parallel-scaling-inside-a-database.md` — why 16 workers on a shared DuckDB instance only delivered ~5× not 16×; fixed per-query setup, LogManager mutex, allocator churn, P/E cores; the shared-vs-separate isolation technique; measure-gate discipline from B4/B5.
9. `08-the-design-interview-drill.md` — converts the repo's optimization loops, edges ledger, verification regime, and war stories into interview-shaped question/answer drills so you can construct answers under pressure rather than recite.

## One-line hook per document

- 00: "Here is the exact line in the repo for every C++ construct you will meet."
- 01: "A .duckdb_extension is dlopen into the engine; ParserExtension exists only on the C++ side."
- 02: "Ask 'which phase am I in?' before any callback; both B3 disasters were phase mistakes."
- 03: "The binder already holds the ClientContext mutex; a fresh Connection is a separate transaction."
- 04: "Emit cardinality 0 to terminate; the first version never did and the executor never returned."
- 05: "The C router is the compiled form of framework.sql's matched + QUALIFY + validation CTEs; statics bypass the DB entirely."
- 06: "Read the first error. Touch one .cpp. Rebuilds are fast because the DuckDB submodule is already built."
- 07: "Serial rate × workers is the naive model; the wall is DuckDB's per-task OLAP setup under micro-OLTP churn from many conns."
- 08: "The war stories, the B4/B5 gates, and the edges ledger are the interview ammunition; numbers without the mechanism lose to the follow-up."

## Tone and invariants of this track

- Mechanism first. "What" is always followed by "why the engine does it this way" and "what broke the last time someone got it wrong" (the repo contains the scars).
- No cheerleading. The hand-rolled string parser, bind-time discovery limitations, and the permanent transaction split for `CREATE ROUTE` are called hacks where they are hacks.
- Every "Read it yourself" section is an ordered list of real `file:line` pointers plus 2–4 interview-style questions you should be able to answer after reading the surrounding code.
- The pure-SQL mental model (FastAPI equivalents, table macros, try_cast, MVCC statements, autocommit) is named explicitly so you can map rather than memorize.

Start with 00. Open the files in another pane while you read.
