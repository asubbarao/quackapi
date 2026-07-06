# Discovery Report: DuckDB C++ Extension Path for quackapi Proper-C Track

**Date:** 2026-07-01  
**Agent:** DISCOVERY (grok)  
**Target:** Proper-C track for quackapi (compiled `quackapi.duckdb_extension` doing routing+validation in C; DB executes only rendered handlers). Current Tier-2 uses `ducktinycc` JIT of serve_brain.sql (raw socket `accept` + `pthread` pool + `dlsym` of `duckdb_*` symbols calling the `handle_request` TABLE macro).

**Ground truth files consulted (via shell):**
- `/Users/aloksubbarao/quackapi/README.md`
- `/Users/aloksubbarao/quackapi/edges.md` (esp. Edge #9 Round 3: pure-SQL floor ~1k req/s due to OLAP query shape per request; "the router IS the query"; proper-C target = move match+validation to C, 34k point-query floor)
- `/Users/aloksubbarao/quackapi/framework.sql` (`handle_request` macro + routes/param_schema registry as config-as-data)
- `/Users/aloksubbarao/quackapi/serve_brain.sql` (16-worker C accept loop, header parse, prepared `SELECT * FROM handle_request(?,?,?,?)`, SSE chunking, dlsym for `duckdb_open/connect/prepare/bind/execute/query/...`, per-worker persistent conn + `threads=1`)

**Two tracks restated (from edges.md #9):**
- PURE (this repo): SQL brain + thin C (tinycc) shuttle. ~1k req/s honest ceiling.
- PROPER-C: compiled extension owns routing/validation loop + socket; registry tables remain SQL *data*; DB only runs the final handler point query (already 34k demonstrated via /q2).

## 1. What the C++ API buys quackapi SPECIFICALLY over the C API (evaluated)

**Key sources (fetched):**
- https://duckdb.org/community_extensions/development.html (C++ template is the documented path)
- https://github.com/duckdb/extension-template (CMake + vcpkg + full DuckDB submodule)
- https://github.com/duckdb/extension-template-c (experimental C/C++ API template; "No DuckDB build required")
- https://duckdb.org/docs/current/extensions/overview.html
- https://github.com/duckdb/duckdb/blob/main/test/extension/loadable_extension_demo.cpp (authoritative live usage of ParserExtension, OptimizerExtension, TableFunction, loader.Register*, catalog.Create*, DBConfig)
- DuckDB headers (raw fetches): `parser/parser_extension.hpp`, `optimizer/optimizer_extension.hpp`, `function/table_function.hpp`, `function/replacement_scan.hpp`, `main/database.hpp`, `main/extension_loader.hpp` (via demo includes)

**C++ vs C Extension API distinction (critical):**
- The primary C++ template (`extension-template`) builds *against DuckDB's internal C++ headers* (`duckdb.hpp`, `ScalarFunction`, `TableFunction`, `ExtensionLoader`, full planner/catalog/optimizer/parser).
- The C template (`extension-template-c`) uses the **stable C Extension API** (duckdb_ext_api_v1 + capi headers). Much lighter (no full DuckDB source build for your compile step).
- Current quackapi uses raw C via tinycc (similar spirit to C API: dlsym `duckdb_*`).

**Evaluated capabilities (what each buys quackapi):**

a. **ParserExtension** (`ParserExtension::Register(config, ...)` + `parse_function_t` / `plan_function_t` / `parser_override_function_t` + `SimpleToken` stream + `consumed_tokens`)
   - Allows registering custom syntax that the PEG parser hands off to your code (e.g. `ROUTE GET '/users/{id}' AS SELECT ...` or a whole DSL statement).
   - Returns `ParserExtensionParseData` → your `plan_function` emits a `TableFunction` + params (or statements).
   - **Materially helpful for web framework?** High signal for portfolio/thesis. Enables "native SQL surface" for route registration instead of `INSERT INTO routes ...`. Makes `ROUTE` feel like a first-class statement. Not required for functionality (current data-driven registry already works perfectly). Could be a "proper-C" flourish but does not move the perf needle on hot path (routing still needs to be fast match code). Demo shows exactly `ParserExtension::Register` + a `QuackExtension` that claims tokens.

b. **Replacement scans** (`config.replacement_scans.emplace_back(...)` with `replacement_scan_t` callback; turns unknown table name into a TableRef)
   - Classic use: `SELECT * FROM 'file.csv'` → auto csv_scan without explicit func.
   - **Materially helpful?** Low for a web framework. Could theoretically turn magic names into route handlers or attach virtual "endpoints", but it's a parser/catalog hack. Not core to routing+validation perf. Nice-to-have for ergonomics (e.g. `FROM route('/users')`), but overkill vs current explicit registry.

c. **TableFunction** (full `class TableFunction : ...` with `bind`, `init_global`, `init_local`, `function`, `table_scan_progress`, projection pushdown via `supports_projection_pushdown`, cardinality, filter pushdown, named params, etc. + `loader.RegisterFunction` or catalog.CreateTableFunction)
   - Demo: full `QuackFunction` (bind produces return cols, global state for offset, vectorized `function`).
   - **Materially helpful?** Very. A "serve" surface could be exposed as a table function for diagnostics or control (`SELECT * FROM serve_status()`). More importantly, advanced table funcs give you the full bind/init/execute vectorized contract (the same contract the C API table functions get, but with richer optimizer integration and direct access to LogicalGet etc.). For quackapi proper-C, the real win is *not* making the HTTP server a table function — the server is a background loop. The TableFunction capability means you can also ship rich *data* sources as part of the same extension (e.g. a `routes()` or metrics table func) with pushdown. Current tinycc UDFs are limited (scalar or simple table-returning). Full TableFunction is a material upgrade for any extension that wants to look "native."

d. **Optimizer hooks** (`OptimizerExtension::Register` + `optimize_function` (post) / `pre_optimize_function`)
   - Demo: `RowIdOptimizerExtension` that walks LogicalOperator tree, finds specific LogicalGet, injects a custom filter function expression.
   - **Materially helpful?** Niche-to-none for routing perf. Routing in proper-C happens *before* SQL execution (in the accept worker's C match loop, like uvicorn). Optimizer hooks let you rewrite plans *after* a query is parsed for routes that *are* SQL. Could be used for clever things (e.g. auto-inject auth filters), but not the 20x win. Pure overhead for the "move routing out of SQL" goal.

e. **Storage** (StorageExtension, custom file formats, attachable storage, etc.)
   - Full custom VFS/storage (see delta, spatial, etc.).
   - **Materially helpful?** Zero for a web framework. The "data" is the `routes` + `users` tables in a normal DuckDB file (or :memory:). No need for custom storage engine.

**Summary of "buys specifically":**
- ParserExtension + full TableFunction + direct internal access (catalog, ClientContext, DBConfig, ExtensionLoader.GetDatabaseInstance()) are the real differentiators vs plain C API / tinycc.
- They let you make quackapi *feel* more like a first-class DuckDB citizen (new syntax, rich funcs) and give direct C++ access to the DB instance inside your C worker threads (no dlsym, typed Connection objects, easier handler execution).
- None of them are *required* to hit the uvicorn-class perf target. The perf target is achieved by moving the segment-match + TRY_CAST validation + handler templating out of the per-request SQL CTE into compiled C code that lives in the accept worker. The C++ API makes writing+maintaining that code nicer and gives extra polish (ParserExtension for `ROUTE` syntax would be a killer demo), but the core "proper-C" move is just "compiled code owns the hot match loop."

## 2. Long-lived background TCP accept loop (threads, lifecycle, shutdown) — feasible in C++ extension?

**Yes, feasible and already proven in spirit by quackapi (tinycc pthread pool + detached accept_loop) and by DuckDB core itself (TaskScheduler, httpfs connection pools, etc. use threads).**

**How it would look in C++:**
- In `Load(ExtensionLoader &loader)` (or the `DUCKDB_CPP_EXTENSION_ENTRY` entry), capture `auto &db = loader.GetDatabaseInstance();`
- Spawn `std::thread` (or pthread) for accept_loop + worker pool exactly as serve_brain does.
- Workers obtain `Connection con(db);` (or raw duckdb_connect) once per worker at startup, prepare the handler SQL (or the thin "execute pre-rendered handler" path), set `threads=1`, load shellfs/curl_httpfs etc.
- The loop itself can be *identical* raw socket code (or slightly C++-ified).
- Return immediately from the load/start function (`"LISTENING_IN_BACKGROUND"`).

**Idiomatic?** Not really. DuckDB extensions are query augmenters, not servers. Core team notes DuckDB is "in-process ... not a database server" (background threads discussion). But technically supported — extensions run in the host process address space.

**Gotchas with DuckDB threading / instance lifecycle (concrete):**
- **DatabaseInstance lifetime**: The DB owns the TaskScheduler, BufferManager, etc. Threads must not outlive the instance in a way that touches destroyed state. Use `weak_ptr` or explicit stop token + join on extension unload / DB shutdown.
- **No built-in "extension shutdown hook" exposed cleanly in the public template path.** You register on load; you are responsible for cleanup. (Core has `ExtensionManager`, but relying on it is internal.)
- **Unload / multiple DBs**: LOAD is per-process for the binary, but state is often per-DatabaseInstance. A global listener fd is ok for the "one DB = the server" model quackapi uses today. Multiple concurrent DBs + one listener needs care.
- **SIGPIPE / signals**: Same as today (ignore 13).
- **Process model**: For a long-lived server process (the intended quackapi Tier-2 use), this is fine and exactly what serve_background/forever already demonstrate. For short-lived CLI use the same caveats apply (process must stay alive).
- **DuckDB TaskScheduler interaction**: Setting `threads=1` per worker connection (as quackapi does) remains necessary to avoid thrashing the global scheduler with 16 tiny point queries.
- **Crashes**: A C++ bug still takes down the whole process (no sandbox). Same as tinycc today.
- **Build note**: You get full `<thread>`, `<atomic>`, `std::mutex` etc. for free (C++17).

**Verdict on this axis**: Fully feasible; more pleasant than tinycc (typed C++ instead of string C source + dlsym). The same architectural split (C owns the forever loop + concurrency; SQL brain or pre-rendered handler does the work) applies. You still have the trilemma from edges #1c unless you also implement a plain HTTP-SQL path inside the extension.

## 3. Build + dist complexity (CMake + vcpkg weight, ABI coupling, community CI)

**Sources:**
- https://github.com/duckdb/extension-template (README + CMakeLists.txt)
- https://duckdb.org/community_extensions/development.html
- https://raw.githubusercontent.com/duckdb/community-extensions/main/UPDATING.md (the authoritative pain document)
- extension-ci-tools and MainDistributionPipeline.yml patterns.

**Concrete weight:**
- **CMake + vcpkg**: Mandatory for the C++ template. vcpkg pinned (specific commit in bootstrap). Template pulls two submodules: full `duckdb/` (the entire engine) + `extension-ci-tools`. `make` (or GEN=ninja) builds a full DuckDB + your extension + unittest. Heavy on disk and first build time. ccache + ninja strongly recommended.
- **Per-DuckDB-version ABI coupling**: *Strong and explicit*. "Extension binaries will only work for the specific DuckDB version they were built for." The `.duckdb_extension` has a metadata footer (version hash + platform) appended by the CI scripts. Mismatch → hard failure on LOAD. The C++ API itself "is not a stable API" (UPDATING.md).
- **Rebuild tax**: Every DuckDB release (or main change that touches extension surface) may require code changes + rebuild. Community process: maintain `main` (for next) + `vx.y-codename` branches; use `ref` + `ref_next` in the community descriptor yml; CI in community-extensions + extension-ci-tools rebuilds for you.
- **Community-extensions CI**: Excellent once set up (GitHub Actions matrix for linux/mac/win + wasm). But you must keep the workflow and submodule in sync. New DuckDB releases trigger mass rebuilds.
- **C template contrast** (https://github.com/duckdb/extension-template-c): "No DuckDB build required". Uses pre-generated capi headers + a lighter Makefile flow + python venv for testing. Much lower weight and (intentionally) more stable surface. Experimental today but noted as potentially the future preferred path for most extensions. quackapi's current tinycc style is spiritually closer to this.

**For quackapi "proper-C"**:
- You will pay the full C++ template tax if you want ParserExtension/Optimizer/etc.
- If the goal is *only* "fast compiled routing + the existing socket loop + call into DB for handlers", the C API template may be sufficient and dramatically lighter (and future-proof against internal C++ churn).
- Dist: once in community, `INSTALL quackapi FROM community; LOAD quackapi;` is the dream (and what edges.md implies for the "real compiled quackapi.duckdb_extension").

## 4. Can C++ still call the same low-level socket/accept code?

**Yes, 100%. No forced rewrite.**

- The serve_brain.sql C source (structs for sockaddr_in, socket/bind/listen/accept/read/write/close, pthread_*, setsockopt, signal, snprintf, strstr, etc. + the dlsym dance for duckdb_*) ports directly into a .cpp file inside the extension.
- In C++ you `#include <sys/socket.h>` (or platform equiv) + `<pthread.h>` or `<thread>`. The code is the same.
- Better: inside workers you can use `duckdb::Connection con(db_instance);` (or the C `duckdb_connect`) directly — no dlsym, no string symbol resolution, full typed access. You still prepare `SELECT * FROM handle_request(?, ?, ?, ?)` (pure track) or the macroless brain_sql equivalent, or (proper-C) you implement the match/validate in C++ and only execute the rendered handler via query.
- You keep all the current tuning (TCP_NODELAY, SIGPIPE ignore, per-worker threads=1, curl_httpfs policy, etc.).
- Streaming/SSE chunking, WebSocket upgrade code, background dispatch threads — all stay or improve.

The C++ extension gives you a *compiled, versioned, distributable* version of what tinycc is JITing today, plus the full internal API surface.

## MINIMAL concrete C++ skeleton

An extension that:
- Registers a simple TableFunction (`background_echo(n)`) using the modern `ExtensionLoader` + `TableFunction` pattern (from demo + waddle).
- On load, spawns one detached background `std::thread` that runs a trivial counter loop (illustrates long-lived thread + safe access to DB instance). The thread can be extended to a real accept loop.

```cpp
// quackapi_skeleton.cpp
#define DUCKDB_EXTENSION_MAIN
#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <thread>
#include <atomic>
#include <chrono>

namespace duckdb {

static std::atomic<bool> g_running{false};
static std::atomic<uint64_t> g_counter{0};
static std::thread g_bg;

struct BgState : public GlobalTableFunctionState {
    idx_t offset = 0;
};

static unique_ptr<FunctionData> echo_bind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
    names.emplace_back("counter");
    return_types.emplace_back(LogicalType::BIGINT);
    return nullptr;
}

static unique_ptr<GlobalTableFunctionState> echo_init(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<BgState>();
}

static void echo_func(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &state = data_p.global_state->Cast<BgState>();
    auto &out = output.data[0];
    idx_t count = 0;
    while (count < STANDARD_VECTOR_SIZE) {
        int64_t val = g_counter.load();
        FlatVector::GetData<int64_t>(out)[count] = val;
        count++;
        state.offset++;
        // In real use: read from a queue populated by accept thread
    }
    output.SetCardinality(count);
}

static void bg_loop() {
    g_running = true;
    while (g_running) {
        g_counter++;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Real version:
        //   int c = accept(...);
        //   // parse, match route in C, prepare/execute only the handler on a worker conn
        //   // respond
    }
}

static void LoadInternal(ExtensionLoader &loader) {
    // 1. Register a TableFunction (demonstrates the C++ TableFunction surface)
    TableFunction echo("background_echo", {}, echo_bind, echo_func);
    echo.init_global = echo_init;
    loader.RegisterFunction(echo);

    // 2. Spawn background thread on load (the key for serve_brain equivalent)
    if (!g_bg.joinable()) {
        g_bg = std::thread(bg_loop);
        g_bg.detach();
    }

    // Access to the instance (for real server: store it, hand to workers)
    // auto &db = loader.GetDatabaseInstance();
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackapi_skeleton, loader) {
    duckdb::LoadInternal(loader);
}
}
```

**CMake fragment (from template pattern):**
```cmake
set(EXTENSION_SOURCES src/quackapi_skeleton.cpp)
build_static_extension(quackapi_skeleton ${EXTENSION_SOURCES})
build_loadable_extension(quackapi_skeleton " " ${EXTENSION_SOURCES})
```

Build/run flow is exactly the template: `make`, `./build/release/duckdb -c "LOAD './build/release/extension/quackapi_skeleton/quackapi_skeleton.duckdb_extension'; SELECT * FROM background_echo();"`

To turn this into proper-C quackapi:
- Port the entire serve_brain C logic (socket structs, queue, worker_main with persistent conn + prepared stmt or direct handler exec) into the bg thread(s).
- Implement the route match + validation + OpenAPI bits from framework.sql as C/C++ code over the `routes` + `param_schema` tables (loaded once at startup via a connection).
- Keep `handle_request` for Tier-1 purity and for the "rendered handler" path.

## URLs cited (all fetched in this session)
- https://duckdb.org/community_extensions/development.html
- https://github.com/duckdb/extension-template
- https://github.com/duckdb/extension-template-c
- https://duckdb.org/docs/current/extensions/overview.html
- https://raw.githubusercontent.com/duckdb/community-extensions/main/UPDATING.md
- https://github.com/duckdb/duckdb/blob/main/test/extension/loadable_extension_demo.cpp
- https://github.com/duckdb/duckdb/blob/main/src/include/duckdb/parser/parser_extension.hpp
- https://github.com/duckdb/duckdb/blob/main/src/include/duckdb/optimizer/optimizer_extension.hpp
- https://github.com/duckdb/duckdb/blob/main/src/include/duckdb/function/table_function.hpp
- https://github.com/duckdb/duckdb/blob/main/src/include/duckdb/main/database.hpp (and extension_loader paths)
- https://github.com/duckdb/extension-template/blob/main/CMakeLists.txt
- https://github.com/duckdb/extension-template/blob/main/src/waddle_extension.cpp

## One-paragraph VERDICT

The C++ extension API uniquely enables quackapi to ship a *real, versioned, community-installable* `quackapi.duckdb_extension` that can register first-class syntax via ParserExtension (`ROUTE GET ...`), expose rich vectorized TableFunctions with pushdown, hook the optimizer, and — most relevantly — embed the entire current serve_brain accept loop + a compiled routing+validation implementation while directly using DuckDB's typed C++ Connection/ClientContext API inside worker threads (eliminating dlsym and tinycc). It gives direct access to DatabaseInstance on load for proper lifecycle ownership. However, this power comes at significant cost: heavy CMake+vcpkg+full-DuckDB-submodule builds, a non-stable internal C++ API, and a strict per-DuckDB-version ABI requiring rebuilds and branch gymnastics on every release (documented as painful in UPDATING.md). The lighter experimental C API template avoids most of the build/ABI weight while still allowing the core "C owns the loop + match, DB only runs handlers" architecture. For quackapi's stated goal (portfolio proof that pure DuckDB + compiled routing can hit uvicorn-class numbers), the C++ path is viable and polished but the extra power (parser/optimizer/storage) does *not* justify the coupling if the only thing needed is a fast compiled match loop + sockets; a C-API-based extension (or even staying with tinycc for the prototype and graduating to a thin C template) would be the lower-friction route to the same 30k+ result while preserving the "no hand-waving, stock extensions today" spirit.

