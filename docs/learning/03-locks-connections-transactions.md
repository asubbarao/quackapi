# 03 — Locks, Connections, and Transactions

This document explains the single most important source of "it works in the shell but deadlocks in the server" and "my DDL survived ROLLBACK" surprises in the quackapi C++ extension.

The key objects are:

- `DatabaseInstance` — one per open database file (or `:memory:`). Process-wide for that DB.
- `Connection` — a handle you create from a `DatabaseInstance`. Owns its own transaction context.
- `ClientContext` — the per-connection (or per-statement-in-some-paths) state + lock. This is what the binder and executor hold while they work.

One process → one `DatabaseInstance` (usually) → many `Connection`s → each with its own `ClientContext`.

## The non-recursive mutex on ClientContext

DuckDB's `ClientContext` protects its own state with a mutex. It is **not** recursive.

When the engine is inside `Binder::Bind` (or executing a statement), it holds that mutex for the duration of the work that logically belongs to "this statement's compilation/execution."

If your code, while holding that mutex, tries to call back into the same `ClientContext` (or the `Connection` that owns it) to run another query, you will deadlock trying to acquire the mutex again.

The sampled stack from the first B3 attempt (edges_round5_draft.md:40):

```
RouteDdlPlan
  ClientContext::Query(...)
    std::mutex::lock()
      __psynch_mutexwait   # asleep forever
```

`RouteDdlPlan` was being called while the binder already held the lock. `ClientContext::Query` tried to take it again.

## How the current code stays alive

### In the ParserExtension plan hook

```cpp
// ext-cpp/src/quackapi_extension.cpp:305
// Plan is bind-time: it must NOT execute SQL or touch tables. Running context.Query()
// here self-deadlocks (the binder already holds the ClientContext lock). Instead the
// plan just packages the parsed DDL as parameters for quack_apply_route; all side
// effects (INSERT/DELETE + g_rt reload) happen at execution time in ApplyRouteFunc.
static ParserExtensionPlanResult RouteDdlPlan(...) {
	...
	result.function = MakeApplyRouteFunction();
	result.parameters.push_back(Value(...));
	// no Query calls
	return result;
}
```

Zero SQL. Only packaging.

### In the table function execute

```cpp
// 378
// Nested SQL must run on a FRESH connection: the executing statement holds this
// context's lock, so context.Query() here deadlocks against ourselves.
Connection con(DatabaseInstance::GetDatabase(context));

... later ...
auto r1 = con.Query(ins_routes);
...
const char* rl = quack_reload_router(dbp.c_str());
```

`DatabaseInstance::GetDatabase(context)` gives you the shared `DatabaseInstance` from inside a `ClientContext`.

`Connection(DatabaseInstance&)` creates a **brand new** `Connection` object with its own `ClientContext` and its own lock. That new connection can call `Query` freely; it is not the one the outer statement is using.

## The honest cost: separate transaction

Because `con` is a fresh `Connection`:

- Its `INSERT` / `DELETE` / `CHECKPOINT` run in a **different transaction** from the statement that contained the `CREATE ROUTE`.
- If the outer statement is inside an explicit `BEGIN; ... CREATE ROUTE ...; ROLLBACK;`, the route rows and the `g_rt` reload will still be visible after the rollback.

See `edges_round5_draft.md:100`:

> Transaction boundary consequence (honest semantic edge).
>
> Because the side-effect SQL runs on its own `Connection`, the route DDL commits in a separate transaction from the statement that issued the `CREATE ROUTE`. A `CREATE ROUTE` inside an outer transaction that later aborts will still persist the route. ... This is a real, permanent semantic edge of the mechanism, not a bug we hide.

FastAPI has the same property (decorators run at import time, outside any "request transaction"), but SQL users have a strong expectation that DDL is transactional with its statement. This implementation technique makes that expectation false.

There is no way around it while using the "create a fresh Connection and run Queries" pattern. A production-grade version would need either:

- An internal API to run DDL on the same context without taking the top-level lock again (if DuckDB ever exposes one), or
- A two-phase registration where the parser extension only records intent and a later catalog or post-commit hook applies it, or
- Accepting that `CREATE ROUTE` is not transactional (and documenting it loudly).

## Mapping to SQL concepts you already own

| Concept you know from SQL          | Equivalent here                                                                 |
|------------------------------------|---------------------------------------------------------------------------------|
| `BEGIN; ... ; COMMIT`              | A `Connection`'s active transaction. Each fresh `Connection` starts its own.    |
| `autocommit` (default in most CLIs)| Most implicit statements run in their own tiny transaction on their connection. |
| Single writer + MVCC readers       | DuckDB's model. The lock on `ClientContext` serializes compilation/execution of statements on that context. |
| `SELECT ... FROM pg_catalog` inside a function | Roughly analogous to running `con.Query(...)` from inside a callback — you are crossing a context boundary. |
| Self-deadlock via advisory locks or recursive trigger | Exact analogue of calling `context.Query()` from inside the binder or table func on the same context. |

## One DatabaseInstance, many connections — the serve brain case

When `serve_brain` runs:

```cpp
// ext-cpp/src/quackapi_brain.cpp:510
const char *serve_brain_impl(...) {
	...
	g_ddb_open(db_path, &g_db);
	...
	if (!g_rt) {
		void *boot = 0;
		if (g_ddb_connect(g_db, &boot) == 0) {
			g_rt = quack_load_registry(boot);
			...
		}
	}
	...
	for(k=0; k < NWORKERS; k++){
		... pthread_create worker_main ...
	}
	... accept_loop ...
}
```

Each worker does:

```cpp
// 490
static void *worker_main(void *arg){
	void *con = 0;
	if(g_ddb_connect(g_db, &con) != 0) return 0;
	...
	for(;;){
		... take fd from queue ...
		handle_conn_on(con, fd);   // reuses the *same* worker connection for many requests
	}
}
```

- `g_db` is the single `duckdb_database` handle (one `DatabaseInstance`).
- Each of the 16 workers has its own `duckdb_connection` (its own `ClientContext` + lock).
- The accept thread only accepts sockets; workers do the actual DuckDB work.
- Because each worker has a persistent connection, prepared statements and `SET` state (threads=1, curl_httpfs, etc.) are set once per worker.

This is why keep-alive matters for the ab numbers in B2_RESULT.md: opening a new TCP connection per request would also open/close DuckDB connections; persistent HTTP keep-alive + persistent worker connections removes that cost.

## Cross-process visibility of g_rt

`g_rt` is a plain C `static RouteTable*` at file scope in the loaded extension:

```cpp
// brain.cpp:200
/* B2 globals */
static RouteTable *g_rt = NULL;
```

It lives in the address space of the process that did the `LOAD` (or the first `serve_brain` / `quack_init_router`).

Another `duckdb` process (another CLI, another server binary, a Python interpreter with its own DuckDB) maps its own copy of the extension .so. Its `g_rt` is a different piece of memory. It will only see routes that it loaded itself from the `routes` / `param_schema` tables (the durable single source of truth).

This is documented in edges_round5_draft.md:99:

> `g_rt` (the compiled `RouteTable`) is per-process (a static in the loaded extension). ... Another process ... will not see the new route until it has done its own `LOAD` + `quack_init_router` ...

The tables are shared (via the DB file + WAL). The in-memory compiled router is not.

## Why the "fresh Connection" pattern appears in two places

1. `ApplyRouteFunc` — side effects for `CREATE` / `DROP ROUTE`.
2. (Historically) various test and bootstrap paths that needed to run queries while a statement was in flight.

Any time you are inside a DuckDB callback that is documented as "holding the ClientContext lock" (binder callbacks, table function bind/init/execute on the invoking context, scalar functions in some expression contexts), assume you need a fresh `Connection` for any nested SQL that must be isolated.

The brain workers do **not** need fresh connections for the handler SQL because the handler SQL is executed on the worker's own persistent connection via the low-level `g_ddb_query(con, dec.handler_sql, ...)` path. That connection is not the one holding a binder lock at that moment; it is a top-level query from the worker's perspective.

## Read it yourself

1. `ext-cpp/src/quackapi_extension.cpp:372` (the comment block starting "Nested SQL must run on a FRESH connection").
2. `ext-cpp/src/quackapi_extension.cpp:378` — `Connection con(DatabaseInstance::GetDatabase(context));`
3. `ext-cpp/src/quackapi_brain.cpp:200` — `static RouteTable *g_rt = NULL;`
4. `ext-cpp/src/quackapi_brain.cpp:510` — the `g_ddb_open` + worker connection setup in `serve_brain_impl`.
5. `ext-cpp/src/quackapi_brain.cpp:490` — `worker_main` and the single `g_ddb_connect` per worker.
6. `edges_round5_draft.md:39` — full deadlock explanation and the two latent bugs.
7. `edges_round5_draft.md:99` — transaction boundary + cross-process paragraphs.

## Comprehension questions

1. Open `ApplyRouteFunc`. On the line that constructs `Connection con(...)`, what is the relationship between `context` (the parameter) and the new `con`? Which one holds the lock for the statement that called `quack_apply_route`?
2. After `CREATE ROUTE foo ...;` inside `BEGIN; ... ; ROLLBACK;`, is the route visible to a subsequent `SELECT * FROM routes` in a different connection? Point at the code that makes it so (or the comment that admits it).
3. In the 16-worker brain, how many distinct `ClientContext` locks exist for the served database? Can two workers deadlock each other on DuckDB locks when handling concurrent requests? (Consider the `SET threads=1` they each issue.)
4. Why does `g_rt` being a process-static mean that `quack_reload_router` must be called in *every* process that wants to see a newly `CREATE ROUTE`'d route, even if they all point at the same `.db` file on disk?
