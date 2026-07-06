# 04 — The Table Function Protocol

DuckDB table functions are the extension mechanism for "something that produces rows when scanned, but is not a base table."

You already know SQL table macros deeply:

```sql
CREATE OR REPLACE MACRO handle_request(...) AS TABLE ( WITH ... SELECT ... );
```

A table macro is expanded at bind time into the query plan. It is pure SQL, fully visible to the optimizer, and has no custom C++ lifecycle.

A C++ `TableFunction` is lower level. You control bind, init, and the repeated execution callback. You are responsible for emitting chunks correctly. The engine treats you as an opaque row source.

## The three callbacks

From the registration:

```cpp
// ext-cpp/src/quackapi_extension.cpp:492
static TableFunction MakeApplyRouteFunction() {
	TableFunction tf("quack_apply_route",
		{LogicalType::VARCHAR, LogicalType::VARCHAR, ...},   // 8 input parameters
		ApplyRouteFunc,                                       // the execute callback
		ApplyRouteBind);
	tf.init_global = ApplyRouteInit;
	return tf;
}
```

### 1. Bind — declare the result schema and capture parameters

```cpp
// 342
static unique_ptr<FunctionData> ApplyRouteBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ApplyRouteBindData>();
	bind_data->action = input.inputs[0].GetValue<string>();
	... copy all 8 ...
	names.emplace_back("status");
	return_types.emplace_back(LogicalType::VARCHAR);
	return std::move(bind_data);
}
```

- Runs during the bind phase.
- You mutate the output `return_types` and `names` vectors to tell the engine what your scan will produce.
- You return a `unique_ptr<FunctionData>` (your `ApplyRouteBindData`) that will be passed back to you in every execute call.
- You may not emit rows here. You are still compiling.

### 2. Init — allocate per-scan state

```cpp
// 360
struct ApplyRouteGlobalState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<GlobalTableFunctionState> ApplyRouteInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<ApplyRouteGlobalState>();
}
```

- Called once, after bind, before any rows are pulled.
- `GlobalTableFunctionState` is for state that lives for the lifetime of this particular scan of the table function.
- Local state (per-thread) also exists in the full API (`init_local`), but `quack_apply_route` is simple and only uses global.
- This is the right place to open files, allocate buffers, reset counters, etc.

### 3. Execute — produce chunks, repeatedly, until you say stop

```cpp
// 368
static void ApplyRouteFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<ApplyRouteGlobalState>();
	if (gstate.done) {
		output.SetCardinality(0);
		return;
	}
	gstate.done = true;

	auto &bind = data_p.bind_data->Cast<ApplyRouteBindData>();
	... compute result_status ...

	output.SetCardinality(1);
	output.data[0].SetValue(0, Value(result_status));
}
```

The contract, stated precisely:

- DuckDB calls your execute function.
- You fill `output` (a `DataChunk`) with 0 or more rows.
- You call `output.SetCardinality(N)`.
- The executor appends those rows to the result and calls you again.
- When you want to signal "no more rows", emit a chunk whose cardinality is 0 (or leave it at 0 and return).
- After that the scan is finished for this invocation.

There is no "return true for done" or "throw a special exception." The 0-cardinality chunk is the sentinel.

## DataChunk and Vector basics (vectorized batches)

A `DataChunk` is a batch of rows in columnar form. Each column is a `Vector`.

- Cardinality can be 0 to `STANDARD_VECTOR_SIZE` (usually 2048).
- For `quack_apply_route` we only ever emit 0 or 1 row because it is a side-effect utility, not a bulk producer.
- `output.data[0].SetValue(0, Value(...))` writes into the first (only) row of the first (only) column.

In a real analytics table function you would typically fill entire vectors with `FlatVector::GetData<T>()` and then `SetCardinality` to the number of valid rows in the batch.

The engine pulls chunks on demand. If your function is used inside a larger query (`SELECT status FROM quack_apply_route(...) JOIN ...`), the join or aggregation will keep pulling until you emit the terminating 0-cardinality chunk.

## The missing done-flag bug (exact failure mode)

The first B3 implementation of `ApplyRouteFunc` (or the plan path) did the work and returned without ever setting a 0-cardinality chunk on a subsequent call.

Result: the executor saw a row, appended it, called the function again, saw another row (or the same state), appended it, ... forever.

Symptoms observed:
- Client never got a result back (hung waiting for end-of-stream).
- Or the process burned CPU emitting infinite copies of the status row until memory or time limits hit.

The fix is the two-line pattern now present:

```cpp
if (gstate.done) {
	output.SetCardinality(0);
	return;
}
gstate.done = true;
... emit one row with cardinality 1 ...
```

Any table function that is not a pure streaming source (i.e., that has finite work) needs an equivalent "have I finished?" guard in its execute callback.

## Contrast with a SQL table macro

| Aspect                        | SQL `CREATE MACRO ... AS TABLE (WITH ...)`          | C++ `TableFunction` (`quack_apply_route`)                  |
|-------------------------------|-----------------------------------------------------|------------------------------------------------------------|
| Expansion                     | At bind time, inlined into the query plan           | Opaque row source; optimizer sees only the declared schema |
| Optimizer visibility          | Full (cardinality estimates, pushdown, etc.)        | Almost none (you are a black box)                          |
| Can run arbitrary C++         | No (only SQL + macros)                              | Yes (fresh Connection, manual string work, pthread state)  |
| How you terminate             | The final SELECT simply returns 0 rows              | You must emit a chunk with `SetCardinality(0)`             |
| Can do side effects           | Yes (but still inside the statement's transaction)  | Yes, but only on a fresh Connection → separate transaction |
| Lifetime of state             | The expanded query tree                             | Your `BindData` + `GlobalState` objects                    |
| When bind runs                | Parse + macro expansion                             | Explicit bind callback                                     |

The pure track (`framework.sql`) could only ever have expressed the routing logic as a table macro or a view over CTEs. It could never have produced a `CREATE ROUTE` surface syntax (that requires `ParserExtension`) and could not have performed the side-effect + reload with a completely separate connection lifecycle.

## How quack_apply_route implements the protocol correctly now

1. Bind always declares exactly one `VARCHAR` column named `status`.
2. Init creates a `GlobalState` with `done = false`.
3. On first execute:
   - Do all the `DELETE` / `INSERT` / `CHECKPOINT` / `quack_reload_router` work on a fresh `Connection`.
   - Write one result row.
   - Set `done = true`.
   - `SetCardinality(1)`.
4. On every subsequent call: `if (done) { SetCardinality(0); return; }`.

This satisfies the pull-until-0 contract and keeps the side effects out of the bind phase.

The same table function is also used directly by `demo_route_syntax.sh` and by anyone who wants the behavior without relying on the `ParserExtension` (the batch `-c` escape hatch).

## Other table functions in the DuckDB source (for further reading)

Inside the submodule you will find many production examples (`read_csv`, `glob`, `range`, etc.). They all follow the same bind → init → repeated-execute-until-0 pattern, just with much larger chunks and more sophisticated state (parallel local state, projection pushdown, filter pushdown).

`quack_apply_route` is intentionally the smallest possible correct implementation of the protocol.

## Read it yourself

1. `ext-cpp/src/quackapi_extension.cpp:492` — `MakeApplyRouteFunction` registration.
2. `ext-cpp/src/quackapi_extension.cpp:342` — entire `ApplyRouteBind`.
3. `ext-cpp/src/quackapi_extension.cpp:360` — `ApplyRouteGlobalState` and `ApplyRouteInit`.
4. `ext-cpp/src/quackapi_extension.cpp:368` — `ApplyRouteFunc` (the complete execute body).
5. `ext-cpp/src/quackapi_extension.cpp:378` — the fresh `Connection` block inside execute (why it is legal here but not in plan).
6. `edges_round5_draft.md:50` — "The table function never signaled end-of-rows."
7. `framework.sql:110` — the signature and first few CTEs of `handle_request` (the thing a table macro *can* express).

## Comprehension questions

1. If `ApplyRouteFunc` omitted the `if (gstate.done)` block entirely and always did the work + `SetCardinality(1)`, what would a `SELECT * FROM quack_apply_route(...)` return to the client, and why would the client appear to hang?
2. Walk from a `CREATE ROUTE` statement to the first row being emitted. Name the exact callbacks in order and the phase each runs in. At which point does the actual `INSERT` happen?
3. Why does a SQL table macro not need an explicit "done" signal while a C++ table function does? What does the macro expansion produce that the engine can see but your `TableFunction` object does not provide?
4. Suppose you wanted `quack_apply_route` to be usable inside a larger vectorized pipeline that expects up to 2048 rows per chunk. What single line in the current execute implementation would have to change, and what would you return instead of `SetCardinality(1)`?
