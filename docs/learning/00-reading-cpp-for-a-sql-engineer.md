# 00 — Reading C++ for a SQL Engineer

This track teaches you to *read* the C++ in quackapi well enough to understand every mechanism, explain it in an interview, and debug phase/lock/ownership bugs without becoming a C++ developer. You already think in sets, CTEs, and self-dispatch. C++ here is the compiled artifact that mirrors your pure-SQL brain (`framework.sql`) while reaching into DuckDB internals that SQL cannot touch.

Every concept below is illustrated with a real excerpt from this repo. Open the file and read the surrounding lines alongside the doc.

## The surface you will touch

All the interesting C++ lives in two files:

- `ext-cpp/src/quackapi_extension.cpp` — the DuckDB extension surface (scalar functions, `ParserExtension`, `TableFunction` glue).
- `ext-cpp/src/quackapi_brain.cpp` — the C accept loop, worker pool, `g_rt` registry, and the routing/validation/templating logic that was ported out of SQL.

The header is tiny:

```cpp
// ext-cpp/src/include/quackapi_extension.hpp:1
#pragma once
#include "duckdb.hpp"

namespace duckdb {

class QuackapiExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	...
};
```

## Headers and includes

```cpp
// ext-cpp/src/quackapi_extension.cpp:1
#define DUCKDB_EXTENSION_MAIN

#include "quackapi_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
...
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>
```

- `"..."` vs `<...>`: quotes are project-local first (your header), angle brackets are system / DuckDB headers.
- You do **not** get the whole world. Every DuckDB internal you reach (ClientContext, Connection, ParserExtensionParseData, etc.) must be included by path.
- Missing an include produces a real compile error at first use, not a runtime surprise.

## Namespaces

```cpp
// ext-cpp/src/quackapi_extension.cpp:29
namespace duckdb {

inline void QuackapiScalarFun(...) { ... }

class RouteDdlExtension : public ParserExtension { ... };

} // namespace duckdb
```

Almost everything that interacts with DuckDB lives inside `namespace duckdb`. The brain file uses a different approach:

```cpp
// ext-cpp/src/quackapi_brain.cpp:30
extern "C" {
/* ... all the ddb_* typedefs, g_rt, quack_route, serve_brain_impl ... */
} // extern "C"
```

The C linkage block exists so the symbols can be resolved by `dlsym` from the running DuckDB process (and so the two .cpp files can call each other across TU boundaries with `extern "C"` forward decls at extension.cpp:23).

## struct vs class

```cpp
// ext-cpp/src/quackapi_extension.cpp:109
struct RouteDdlData : public ParserExtensionParseData {
	string action;
	...
	duckdb::unique_ptr<ParserExtensionParseData> Copy() const override { ... }
};
```

- `struct` here is public by default (historical C habit). Used for plain data carriers and for types that DuckDB's `make_uniq` / `Cast` will touch.
- `class` is used for the extension hook objects that inherit behavior:

```cpp
// header:7
class QuackapiExtension : public Extension { ... };
```

In practice in this repo: `struct` for bind data / parse data / global state; `class` for the registered extension objects.

## References (&) vs pointers (*)

C++ references are non-nullable aliases. Pointers can be null and must be checked.

In the extension surface (clean, modern DuckDB C++ API):

```cpp
// ext-cpp/src/quackapi_extension.cpp:309
static ParserExtensionPlanResult RouteDdlPlan(ParserExtensionInfo *info, ClientContext &context,
                                              duckdb::unique_ptr<ParserExtensionParseData> parse_data) {
```

- `ClientContext &context` — the binder already owns it; you are borrowing.
- `unique_ptr<...>` — ownership transfer discussed below.

In the brain (systems C ported to C++):

```cpp
// ext-cpp/src/quackapi_brain.cpp:210
void handle_conn_on(void *con, int fd){
    ...
    ddb_result rq; int rcq = g_ddb_query(con, "SELECT 42", &rq);
```

- `void *con` is an opaque handle from `duckdb_connect`.
- Lots of `char *`, `char **` because the router does its own string splitting and owns the pieces until `free`.

Rule of thumb you will use for debugging: if you see `&` on a DuckDB engine object in a callback, you are almost always inside a phase that already holds a lock on it.

## unique_ptr + std::move (who frees what)

DuckDB C++ API uses `duckdb::unique_ptr` (their alias for `std::unique_ptr`).

```cpp
// ext-cpp/src/quackapi_extension.cpp:119
auto copy = make_uniq<RouteDdlData>();
copy->action = action;
...
return std::move(copy);
```

```cpp
// 130 (in ToString etc., and in plan)
result.parameters.push_back(Value(...));
return result;
```

Ownership transfer is explicit with `std::move`. The thing that receives the `unique_ptr` is now responsible for destruction. If you forget the move or assign without it, you either get a copy (sometimes impossible) or a leak / double-free at shutdown.

Contrast the brain side, which does manual `malloc`/`strdup`/`free`:

```cpp
// brain.cpp:580
char *tmp = strdup(p);
...
free(tmp);
```

There is no GC. The person who `strdup`'d must arrange the `free`, or you leak per-request memory under load. The C port deliberately mirrors the SQL brain's per-request allocations (the CTEs allocate anyway) but now the lifetime is visible in C stack frames.

## Lambdas

The DuckDB vectorized execution API is full of them:

```cpp
// ext-cpp/src/quackapi_extension.cpp:32
UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
	return StringVector::AddString(result, "quackapi " + name.GetString());
});
```

This is the C++ equivalent of a SQL lambda inside `list_transform`. The capture (`[&]`) is by reference into the current scope — convenient, but you are responsible for what that reference outlives.

The brain has almost no lambdas; it is a straight port of imperative string logic that used to live in `framework.sql` CTEs.

## extern "C" and why the brain exposes C symbols

```cpp
// extension.cpp:23
// Forward decls from quackapi_brain.cpp (extern C impls)
extern "C" const char* quack_serve_brain(int port, const char* db_path);
extern "C" int quack_block_forever(int x);
extern "C" const char* quack_init_router(const char* db_path);
extern "C" const char* quack_route_decision(const char* method, const char* path, const char* headers, const char* body);
```

```cpp
// extension.cpp:579
extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(quackapi, loader) {
	duckdb::LoadInternal(loader);
}

}
```

```cpp
// brain.cpp:1178 (bottom)
extern "C" const char* quack_serve_brain(int port, const char* db_path) {
  return serve_brain_impl(port, db_path);
}
```

Why? The scalar UDF `ServeBrainFun` calls into the C impl. The worker threads inside the brain resolve DuckDB symbols at runtime with `dlsym(RTLD_DEFAULT, "duckdb_query")` etc. (brain:130). C linkage gives stable, unmangled names that survive across the extension boundary and the dlsym path. The pure-SQL track never needed this because everything was already inside one DuckDB session.

## std::string manipulation (the hand-rolled parser tax)

The `RouteDdlParse` function is a deliberately small hand parser:

```cpp
// extension.cpp:139
static string to_upper(string s) {
	for (auto &c : s) c = (char)toupper((unsigned char)c);
	return s;
}

static string trim(const string &s) { ... }

static string rtrim_semi(const string &s) {
	string t = s;
	while (!t.empty() && (t.back() == ';' || isspace((unsigned char)t.back()))) t.pop_back();
	return t;
}
```

Later:

```cpp
// 160
if (qu.find("CREATE ROUTE ") == 0) { is_create = true; pos = 13; }
...
size_t qstart = rest.find('\'', meth_pos + meth.size());
...
pat = rest.substr(qstart + 1, qend - qstart - 1);
```

This is **not** a production-grade SQL grammar extension. A real one would register new statement kinds with the parser's token stream and produce proper AST nodes. Here we slice strings because the goal was "exact syntax that matches the B2 mental model" with minimal new code. Error messages are poor and the `-c` batch limitation (parse time vs load time) is a direct consequence of doing string matching instead of hooking the real parser.

The brain has even more hand string code (split_segments, parse_query, escape, templating replaces) because that logic moved from SQL `list_filter` / `list_transform` / `replace` into C for the 15-30x win.

## Mental model translation (your existing SQL world)

| SQL / Python thing          | C++ equivalent here                                      | What goes wrong if you get the rule wrong |
|-----------------------------|----------------------------------------------------------|-------------------------------------------|
| `WITH ... SELECT` scope     | `unique_ptr` + move, or raw pointer lifetime             | Use-after-free or leak under concurrent requests |
| `try_cast(x AS INT)`        | `quack_parse_int` / `strtoll` + explicit error codes     | Silent wrong values or missed 422s |
| Macro expansion at call time| `quack_route` called on every worker request             | Hot path cost (the whole point of B2) |
| `CREATE TABLE` DDL txn      | `Connection con(DatabaseInstance::GetDatabase(ctx))` + separate `Query` | DDL commits even if outer txn aborts |
| Autocommit per statement    | Each fresh `Connection` is its own transaction           | "My CREATE ROUTE survived ROLLBACK" surprise |
| Self-dispatch recursion     | Calling `context.Query()` from inside Binder or table func | Immediate deadlock on the non-recursive mutex |

## What this repo deliberately does not do

- No `new` / `delete` of DuckDB objects (always `make_uniq` / DuckDB's allocators).
- No smart pointers around the brain's `RouteDef` arrays (manual `calloc` + `free_route_table`).
- No RAII wrappers for the pthread queue (raw mutex/cond + init once).
- The string parser in `RouteDdlParse` is a hack that works for the documented grammar.

These are visible in the code and are the exact places an interviewer will ask "what would the production version look like?"

## Read it yourself (ordered)

1. `ext-cpp/src/quackapi_extension.hpp:1` — the entire public surface of the extension.
2. `ext-cpp/src/quackapi_extension.cpp:23` — the four `extern "C"` forward decls.
3. `ext-cpp/src/quackapi_extension.cpp:29` — first `namespace duckdb` and the scalar lambdas.
4. `ext-cpp/src/quackapi_extension.cpp:109` — `struct RouteDdlData`.
5. `ext-cpp/src/quackapi_extension.cpp:309` — `RouteDdlPlan` signature (note the `&` and `unique_ptr`).
6. `ext-cpp/src/quackapi_brain.cpp:30` — start of the giant `extern "C"` block.
7. `ext-cpp/src/quackapi_brain.cpp:1178` — the tiny C wrappers at the bottom.
8. `ext-cpp/src/quackapi_extension.cpp:579` — `DUCKDB_CPP_EXTENSION_ENTRY`.

## Comprehension questions (be able to answer out loud)

1. Why are the brain functions declared `extern "C"` in the extension TU and defined with `extern "C"` wrappers at the bottom of the brain TU? What breaks if the linkage is omitted?
2. Walk line 309 of extension.cpp. Identify every ownership or lock implication of the parameter types. Which object owns the `ClientContext` at the moment `RouteDdlPlan` is called?
3. In `RouteDdlParse`, when does `return ParserExtensionParseResult();` (empty) occur, and what does that mean for the core parser? Contrast with returning a `make_uniq<RouteDdlData>()`.
4. Find three places in the brain that do `free` or `g_ddb_free`. For each, point at the matching allocation site. What happens to a worker under load if one of those frees is removed?
5. The hand-rolled `trim` / `find` / `substr` parser for CREATE ROUTE lives only in C++. Why could the pure-SQL track (`framework.sql`) never have expressed an equivalent surface extension?
