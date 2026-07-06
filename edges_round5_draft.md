# Round 5 — CREATE ROUTE — extending SQL itself (DRAFT)

**DRAFT — pending merge into edges.md.** Test-result specifics live in ext-cpp/B3_VERIFY_RESULT.md.

This is the edge that justified choosing the full C++ API track (CMake + vcpkg + DuckDB submodule pinned to the exact version) over the stable C ABI. DuckDB's `ParserExtension` hook — the mechanism that lets an extension claim a statement string the core parser rejects, return custom parse data, and plan it — exists only in the unstable C++ API, not the C ABI.

**Thesis.** Extending SQL's *surface* is easy (the parse is string slicing + a small hand grammar). The cost lives in the *execution model* underneath: lock phases (bind vs execute), the table-function chunk-emission protocol, and transaction boundaries. In the pure-SQL track the framework could never even attempt this. In C++ it is possible, but the engine's internals bill you immediately for every shortcut.

## 10. `CREATE ROUTE` / `DROP ROUTE` as first-class DDL — FastAPI decorators reimagined as SQL

**Hypothesis:** the registration surface can be DDL instead of imperative inserts or decorators.

```sql
CREATE ROUTE get_user GET '/users/{id}' (id INT) AS
  SELECT to_json(u) AS body FROM users u WHERE u.id = {id};

CREATE ROUTE create_user POST '/users'
  (name TEXT, age INT)
  STATUS 201
AS
  INSERT INTO users(name, age) VALUES ({name}, {age})
  RETURNING to_json(users) AS body;

CREATE ROUTE search GET '/search' (q TEXT, limit INT <= 100 ?) AS
  SELECT ... LIMIT coalesce({limit}, 100);

DROP ROUTE get_user;
```

Param rules (identical to the prior B2 C router and the pure-SQL brain):
- `{name}` in the pattern → path param (always required).
- Otherwise: GET/DELETE → query param; POST/PUT/PATCH → body param.
- Trailing `?` on a non-path param → optional.
- `<= N` / `>= N` → `{"le":N}` / `{"ge":N}` in `constraint_json`.
- The `routes` and `param_schema` tables remain the single source of truth; the syntax is sugar that performs the INSERTs (plus location/required/constraint inference) and then forces a reload of the in-process C route table (`g_rt`).

**Probe.** The first implementation deadlocked 100% of the time.

Mechanism (precisely): a `ParserExtension`'s `plan_function` runs inside `Binder::Bind`. At that point the engine already holds the `ClientContext` lock (a non-recursive mutex). The initial code called `ClientContext::Query()` on that same context to run its INSERTs + reload:

```
RouteDdlPlan
  ClientContext::Query(...)
    std::mutex::lock()
      __psynch_mutexwait   # asleep forever
```

The identical re-entrant pattern was also present in the table function's execution callback (the "portable fallback"), so that path hung identically.

Two more latent bugs behind the deadlock:

- The plan returned an empty `TableFunction` object (nothing for the executor to invoke).
- The table function never signaled end-of-rows. DuckDB's chunk protocol for table functions calls the function repeatedly until it emits a chunk with cardinality 0. No `done` flag + `SetCardinality(0)` meant infinite emission.

**The fix pattern (generalizable):** side effects belong at *execution time*, not bind time; and any nested SQL executed from inside engine callbacks must run on a *fresh* `Connection` to the same `DatabaseInstance` (its own `ClientContext` = its own lock and its own transaction).

Current shape (after the fix):

- `RouteDdlParse` does the claim at parse time (a minimal hand parser for the exact grammar; returns `ParserExtensionParseResult` with a `RouteDdlData` or empty to let the core parser run).
- `RouteDdlPlan` only packages the fields as parameters to `quack_apply_route(...)` and returns a proper `TableFunction` descriptor. Zero SQL, zero side effects.
- `ApplyRouteFunc` (the execution callback) does:

  ```cpp
  Connection con(DatabaseInstance::GetDatabase(context));  // fresh
  ...
  con.Query("DELETE FROM ...");
  con.Query("INSERT INTO routes ...");
  ... 
  quack_reload_router(dbp.c_str());
  ```

  A `ApplyRouteGlobalState { bool done = false; }` plus early `SetCardinality(0)` on the second call satisfies the chunk protocol.

The `quack_apply_route` table function is also exposed directly so scripts and `-c` batches have a non-syntax path that still exercises the same INSERT+reload logic.

**Result (observed behavior).** In an interactive shell or line-at-a-time input:

- `LOAD 'quackapi_extension';` then `CREATE ROUTE ...;` succeeds, populates both tables, calls reload, and the route is immediately visible to the C router (and therefore to `serve_brain` workers).
- `DROP ROUTE` symmetrically removes the row(s) and reloads.
- Param constraints, 422 detail generation, location inference, and handler templating (`{id}` → literal in the emitted SQL) all match the prior B2 implementation and the pure-SQL `handle_request` oracle.
- Byte parity on the SSOT rows (when summary is set to route_id) holds against the equivalent `register_route(...)` + manual `param_schema` insert.

**Parse-order edge (the observable phase boundary).**

A single CLI batch:

```
duckdb :memory: -c "LOAD 'quackapi_extension'; CREATE ROUTE ...;"
```

fails with a core parser error ("at or near ROUTE"). The whole `-c` string is parsed *before* any statement executes, so the `ParserExtension` is not yet registered when the `CREATE ROUTE` token is seen. Statement-at-a-time input (REPL after a prior `LOAD`, or piped stdin with LOAD already processed) works. Lesson: syntax extensions are registered at *runtime* (LOAD) but consulted at *parse time*. The boundary between those two phases is user-visible and cannot be papered over by the extension author.

The portable escape for batch scripts is `SELECT * FROM quack_apply_route('CREATE', ...)` (or the equivalent for DROP), which does not rely on the parser hook.

**Transaction boundary consequence (honest semantic edge).**

Because the side-effect SQL runs on its own `Connection`, the route DDL commits in a separate transaction from the statement that issued the `CREATE ROUTE`. A `CREATE ROUTE` inside an outer transaction that later aborts will still persist the route. FastAPI has no transactional route registration either (decorators run at import time), but SQL users reasonably expect DDL to be transactional with the statement that contains it. This is a real, permanent semantic edge of the mechanism, not a bug we hide.

**Cross-process limit.**

`g_rt` (the compiled `RouteTable`) is per-process (a static in the loaded extension). The `routes`/`param_schema` tables are the durable shared truth. Another process (another `duckdb` CLI, another server instance) will not see the new route until it has done its own `LOAD` + `quack_init_router` (or `serve_brain` boot). In a multi-process deployment you still write the tables and arrange for each process to reload on its own schedule.

**Why this could never have existed in the pure-SQL track.**

The pure track has only `INSERT INTO routes SELECT * FROM register_route(...)` (plus manual `param_schema` rows) executed at load time, or the equivalent JSON seed. There is no `ParserExtension` hook, no way to claim new top-level syntax, and no way to make `CREATE ROUTE` and `DROP ROUTE` first-class DDL. The surface extension is C++-only by construction.

**Verdict: REAL (C++-only, with execution-model and phase edges).**

The feature works: the decorator syntax is now DDL, the SSOT tables stay authoritative, g_rt is kept in sync, behavior and parity are preserved, and the hot path numbers from Round 4 are untouched. But the engine's internal accounting is immediately visible in every shortcut:

- bind-time vs execute-time lock separation,
- table-function "emit 0 rows to terminate" protocol,
- fresh-Connection rule for nested work,
- parse-time registration vs execution-time effect,
- separate transaction for the DDL side effects.

These are not implementation bugs that will be cleaned up later; they are the price of reaching into the parser and binder from an extension. The pure-SQL track is defeated from even trying; the C++ track makes the attempt possible and then makes the cost of the attempt legible. That is exactly the kind of edge this ledger exists to document.

Honest caveats (these are real):

- The parser is a small hand-rolled slicer, not a full grammar extension; error quality is basic.
- No new catalog object is created; `routes` and `param_schema` remain ordinary tables.
- The transaction split is permanent for this implementation technique.
- Cross-process visibility still requires explicit reload per process.
- The `-c` batch parse-order limitation is inherent to when extensions are loaded vs when statements are parsed.

The syntax extension is complete for the documented grammar. What it surfaces is how much of DuckDB's execution model becomes your problem the moment you decide to own part of the language surface.