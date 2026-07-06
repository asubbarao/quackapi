# 02 — The Life of a Query (and Where Your Extension Hooks Fit)

DuckDB, like every SQL engine, executes a statement in distinct phases. The phase you are in determines what you are allowed to do, what locks are held, and whether the catalog / parser state you are looking at is complete.

If you internalize only one habit from this track, make it this:

> Before writing or debugging any extension callback, ask: "Which phase am I in?"

Both B3 failure classes were phase bugs.

## The four phases (concrete in DuckDB)

1. **Parse**  
   The string is turned into a parse tree (or rejected).  
   `ParserExtension::parse_function` is consulted here.

2. **Bind** (sometimes called "plan" in extension APIs)  
   Names are resolved, types are inferred, the logical plan is built, and binder locks are taken.  
   `ParserExtension::plan_function` runs inside `Binder::Bind`.  
   Table function `bind` callback also runs here.

3. **Optimize / physical planning**  
   The logical plan is turned into an executable pipeline. You are rarely directly involved.

4. **Execute**  
   Morsels / chunks flow through the pipeline.  
   Table function `init` + repeated calls to the execute function happen here.

These phases are not an academic diagram. They are visible in the source as separate callbacks with different contracts.

## Where ParserExtension hooks fire

```cpp
// ext-cpp/src/quackapi_extension.cpp:330
class RouteDdlExtension : public ParserExtension {
public:
	RouteDdlExtension() {
		parse_function = RouteDdlParse;
		plan_function = RouteDdlPlan;
	}
};
```

Registered once at load time:

```cpp
// 560
ParserExtension::Register(config, RouteDdlExtension());
```

### Parse hook

```cpp
// 146
static ParserExtensionParseResult RouteDdlParse(ParserExtensionInfo *info, const string &query) {
	string q = trim(query);
	...
	if (qu.find("CREATE ROUTE ") == 0) { ... return ParserExtensionParseResult(std::move(data)); }
	...
	return ParserExtensionParseResult(); // let core parser handle / error
}
```

If your function returns a non-empty result, you have **claimed** the statement. The core parser will not see it. If you return empty, the statement falls through exactly as if your extension did not exist.

This is why a single CLI batch:

```
duckdb :memory: -c "LOAD 'quackapi_extension'; CREATE ROUTE ...;"
```

fails with "at or near ROUTE". The entire `-c` argument is parsed in one go **before** any statement executes and therefore before your `LOAD` has run `ParserExtension::Register`. See `edges_round5_draft.md:89`:

> Parse-order edge (the observable phase boundary). ... The whole `-c` string is parsed *before* any statement executes, so the `ParserExtension` is not yet registered when the `CREATE ROUTE` token is seen.

Statement-at-a-time input (REPL after LOAD, or piped input) works because each line is parsed after the previous one has executed.

### Plan hook (inside bind)

```cpp
// 310
static ParserExtensionPlanResult RouteDdlPlan(ParserExtensionInfo *info, ClientContext &context,
                                              duckdb::unique_ptr<ParserExtensionParseData> parse_data) {
	...
	result.function = MakeApplyRouteFunction();
	result.parameters.push_back(Value(data.action));
	...
	result.requires_valid_transaction = false;
	return result;
}
```

The comment immediately above it is the key lesson:

```cpp
// 305
// Plan is bind-time: it must NOT execute SQL or touch tables. Running context.Query()
// here self-deadlocks (the binder already holds the ClientContext lock). Instead the
// plan just packages the parsed DDL as parameters for quack_apply_route; all side
// effects (INSERT/DELETE + g_rt reload) happen at execution time in ApplyRouteFunc.
```

`plan_function` runs while the binder holds the `ClientContext` lock. This is the bind phase.

## Where a TableFunction's callbacks fire

The `quack_apply_route` function is the execution vehicle for both the `ParserExtension` rewrite and for direct `SELECT * FROM quack_apply_route(...)` calls.

```cpp
// 492
static TableFunction MakeApplyRouteFunction() {
	TableFunction tf("quack_apply_route",
		{LogicalType::VARCHAR, ...},
		ApplyRouteFunc, ApplyRouteBind);
	tf.init_global = ApplyRouteInit;
	return tf;
}
```

### Bind (still in the bind phase)

```cpp
// 342
static unique_ptr<FunctionData> ApplyRouteBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ApplyRouteBindData>();
	... copy the 8 parameters into bind_data ...
	names.emplace_back("status");
	return_types.emplace_back(LogicalType::VARCHAR);
	return std::move(bind_data);
}
```

Bind runs once per statement (or per call site). It declares the output schema and stashes parameters. It can still see some locks.

### Init (global state, beginning of execute phase)

```cpp
// 365
struct ApplyRouteGlobalState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<GlobalTableFunctionState> ApplyRouteInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<ApplyRouteGlobalState>();
}
```

Init is called once before the first execute. This is where you would open files, allocate per-query state, etc.

### Execute — called *repeatedly*

```cpp
// 368
static void ApplyRouteFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<ApplyRouteGlobalState>();
	if (gstate.done) {
		output.SetCardinality(0);
		return;
	}
	gstate.done = true;

	... do the work, set one row ...
	output.SetCardinality(1);
	output.data[0].SetValue(0, Value(result_status));
}
```

DuckDB's table function protocol is a pull-based chunk iterator:

- The executor calls your function.
- You emit a `DataChunk` with N rows.
- It calls you again.
- You emit another chunk.
- When you emit a chunk with cardinality 0, the scan is considered finished.

There is no "I'm done" return value from the function itself. The 0-cardinality chunk is the signal.

The original B3 implementation forgot the `done` flag + `SetCardinality(0)` path. Result: the executor kept calling forever, emitting the same row or hanging.

## The two B3 phase bugs, exactly as they happened

### Bug A — the `-c` batch limitation (parse vs. execute)

Symptom: `duckdb -c "LOAD; CREATE ROUTE ..."` always fails at the parser.

Root cause: syntax registration (`ParserExtension::Register`) happens at *execution time of LOAD*. The `-c` string is parsed in one shot at *parse time* for the whole batch. The hook was not present when the tokens were seen.

Lesson: anything that affects what the parser accepts is a parse-time registration that must be in place before the first character of a statement is lexed. `LOAD` is an execution-time statement.

Portable workaround that *does* work in `-c`:

```sql
SELECT * FROM quack_apply_route('CREATE', 'name', ...);
```

This path never goes through the `ParserExtension`; the table function is already registered by the time the SELECT is parsed (the LOAD executed before the SELECT in the same batch string? No — wait, same batch string problem. In practice the docs recommend doing the LOAD in one invocation and the apply in a subsequent one, or using interactive/pipe order).

### Bug B — the deadlock (bind vs. execute)

First implementation put the `INSERT INTO routes` + `quack_reload_router` inside `RouteDdlPlan` (or inside the table function without a fresh connection).

Stack from `edges_round5_draft.md:40`:

```
RouteDdlPlan
  ClientContext::Query(...)
    std::mutex::lock()
      __psynch_mutexwait   # asleep forever
```

The mutex is the `ClientContext` lock. It is **non-recursive**. The binder already holds it when your `plan_function` runs. Calling `context.Query()` on the same context tries to acquire it again → deadlock.

The fix (now in the code):

- `RouteDdlPlan` only packages parameters. Zero side effects.
- `ApplyRouteFunc` (execute phase) creates a fresh connection:

```cpp
// 378
// Nested SQL must run on a FRESH connection: the executing statement holds this
// context's lock, so context.Query() here deadlocks against ourselves.
Connection con(DatabaseInstance::GetDatabase(context));
...
con.Query("DELETE FROM ...");
con.Query("INSERT ...");
...
const char* rl = quack_reload_router(dbp.c_str());
```

## The phase lesson, reduced to a checklist

When you are about to do work inside an extension callback, ask:

- Am I in Parse? (I can only claim strings, I cannot resolve names, I cannot run queries, I cannot see the catalog for tables that have not been parsed yet.)
- Am I in Bind/Plan? (I hold locks on the ClientContext. I can declare output types. I must not run queries on the same context. I must not have side effects that other statements in the same transaction will see inconsistently.)
- Am I in Execute? (I can run queries on fresh connections. I must terminate the chunk stream with a 0-cardinality emission. My side effects will be visible immediately to other connections in the same process.)

Every serious bug in the B3 work was a violation of one of those three.

## FastAPI / Python mental model

In FastAPI/Starlette the phases are less visible because everything is Python functions you call at request time:

- "Parse" is just the ASGI scope + path matching (happens per request).
- There is no separate bind phase visible to the app developer.
- Side effects in a dependency or in the route handler run at request time, inside the request's task.

DuckDB makes the phases *first-class* because the SQL compiler has to do work (name resolution, type inference, planning) before it can even start executing, and because it reuses the same `DatabaseInstance` across many statements and connections. Your extension is hooking the compiler, not just a handler.

## Read it yourself

1. `ext-cpp/src/quackapi_extension.cpp:146` — `RouteDdlParse` (the claim / fallthrough).
2. `ext-cpp/src/quackapi_extension.cpp:310` — `RouteDdlPlan` + the big comment above it.
3. `ext-cpp/src/quackapi_extension.cpp:342` — `ApplyRouteBind`.
4. `ext-cpp/src/quackapi_extension.cpp:365` — `ApplyRouteInit` and `ApplyRouteGlobalState`.
5. `ext-cpp/src/quackapi_extension.cpp:368` — `ApplyRouteFunc` (the done + SetCardinality(0) pattern).
6. `ext-cpp/src/quackapi_extension.cpp:378` — the fresh `Connection` construction and the comment.
7. `edges_round5_draft.md:39` — the exact deadlock stack and the two "latent bugs" paragraphs.
8. `edges_round5_draft.md:89` — the parse-order edge for `-c`.

## Comprehension questions

1. In the current code, on which exact line does the decision "this statement will be handled by our table function" become visible to the executor? Is it in parse, plan, or bind of the table function?
2. Suppose someone moves the `quack_reload_router` call from `ApplyRouteFunc` back into `RouteDdlPlan`. Describe the exact deadlock that will occur on the second `CREATE ROUTE` after the first successful one (or on the first, depending on timing). Name the mutex.
3. A table function that emits its single result row and then returns without ever setting cardinality 0 will do what to the client? How would you observe it (as opposed to a plain hang)?
4. Why does the portable escape `quack_apply_route` still have to obey the chunk protocol even though the user called it with `SELECT * FROM ...` rather than via `CREATE ROUTE` syntax? What part of the engine does not know the difference?
