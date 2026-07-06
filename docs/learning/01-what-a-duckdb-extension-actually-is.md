# 01 — What a DuckDB Extension Actually Is

A `.duckdb_extension` file is a dynamic shared library (`.so` on Linux, `.dylib` on macOS, `.dll` on Windows) that DuckDB's loader maps into the running `duckdb` process at `LOAD` time.

Everything else — scalar functions, table functions, `ParserExtension`, new types — is just "code that runs inside the engine's address space after the loader has done `dlopen` + symbol resolution + ABI checks."

## The entry point macro

```cpp
// ext-cpp/src/quackapi_extension.cpp:579
extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(quackapi, loader) {
	duckdb::LoadInternal(loader);
}

}
```

`DUCKDB_CPP_EXTENSION_ENTRY` expands (in the DuckDB headers you build against) to the symbol the loader looks for:

- On load, DuckDB finds `quackapi_duckdb_cpp_init` (or the mangled equivalent) and calls it with an `ExtensionLoader&`.
- `LoadInternal` (defined in the same file) does all the real work: registering scalars, the `quack_apply_route` table function, and the `ParserExtension`.

Contrast a pure C ABI extension (the other flavor). It would use a different macro and could only register C API surfaces. `ParserExtension` does not exist in the stable C ABI.

## What `LOAD '...' ` actually does (at the process level)

1. The CLI / Python / whatever calls into DuckDB's extension loader.
2. Loader computes the expected filename for the platform + DuckDB version.
3. `dlopen` (or `LoadLibrary`) the file into the **current process**.
4. Looks up the init symbol.
5. Calls it. Your `LoadInternal` runs while the engine is still in the "extension initialization" phase.
6. The symbols you registered (functions, parser hooks) become visible to subsequent statements in the same `DatabaseInstance`.

Because it is `dlopen` into the same address space:
- You can call back into DuckDB via `dlsym` (see brain.cpp:130 `resolve_sym`).
- You share the same allocator, the same `DatabaseInstance`, the same locks.
- A segfault in your extension takes down the whole `duckdb` process.
- A deadlock in your callback deadlocks the only thread the client may have.

## Why `-unsigned` and ABI version pinning

DuckDB extensions are **pinned to the exact minor version** of the engine they were compiled against.

Look at the build layout after `make release`:

```
ext-cpp/build/release/repository/v1.5.3/osx_arm64/...
```

The directory name is the DuckDB version the extension was built for. Inside the extension metadata (written by the build system) is an ABI hash / version stamp.

When you do `LOAD 'quackapi_extension'`, the loader:
- Opens the file.
- Reads the metadata.
- Compares against the running DuckDB's version and ABI.
- If mismatch: rejects with a clear error.

This is why the `duckdb/` submodule inside `ext-cpp/duckdb/` must be exactly the v1.5.3 tree that matches the `duckdb` binary you are running. Rebuild the submodule or switch DuckDB version and your LOAD will be rejected even if the .so exists.

The `-unsigned` flag (or equivalent in the Python API) tells the loader "I accept that this extension is not in the official signed catalog." Without it, only extensions that have gone through DuckDB's signing process are accepted.

## The two compiled flavors

DuckDB ships two extension ABIs:

1. **Stable C ABI** (`c_api` / `demo_capi` style).  
   - Limited surface.  
   - Binary compatibility across a wider range of DuckDB versions (within a major).  
   - No `ParserExtension`. No easy access to the binder, the planner, `ClientContext` objects, etc.

2. **Full C++ API** (what quackapi uses).  
   - You `#include "duckdb.hpp"` and the full internal headers.  
   - You get `ParserExtension`, `TableFunction` with custom bind/init/execute, direct access to `DatabaseInstance`, `Connection`, `ClientContext`, expression executors, etc.  
   - You must rebuild against the exact same DuckDB source tree.

quackapi chose the C++ track for one reason only:

> `ParserExtension` exists only in the C++ API.

See `edges_round5_draft.md:1`:

> This is the edge that justified choosing the full C++ API track ... over the stable C ABI. DuckDB's `ParserExtension` hook ... exists only in the unstable C++ API, not the C ABI.

The pure-SQL track (framework.sql + serve_brain.sql + tcc JIT) can never register `CREATE ROUTE` syntax because it has no way to hook the parser. The C++ extension can, at the cost of the phase and lock rules you will learn in the next two documents.

## The tcc-JIT track relation

Earlier iterations (listener_ducktinycc.sql etc.) compiled C source strings at runtime with tinycc and `dlsym`'d the resulting functions into the same process. That is "JIT from SQL string."

A DuckDB C++ extension is "AOT from C++ source to .duckdb_extension artifact, loaded once per process."

Both techniques ultimately give you native code running inside the DuckDB process and calling the same underlying C symbols (`duckdb_query`, etc.). The extension path gives you:
- Real C++ types and the full engine surface.
- Build-time type checking.
- The ability to ship a single artifact instead of shipping .c sources + a JIT.

The cost is exactly the build system (document 06) and the requirement that the artifact match the running engine's version.

## Where the registration actually happens in this repo

```cpp
// ext-cpp/src/quackapi_extension.cpp:510
static void RegisterApplyRoute(ExtensionLoader &loader) {
	loader.RegisterFunction(MakeApplyRouteFunction());
}

static void LoadInternal(ExtensionLoader &loader) {
	... scalar registrations ...

	RegisterApplyRoute(loader);

	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	ParserExtension::Register(config, RouteDdlExtension());
}
```

`ExtensionLoader::RegisterFunction` is the C++ equivalent of `CREATE FUNCTION` or `duckdb_create_scalar_function` in the C API.

`ParserExtension::Register` mutates the `DBConfig` so that future parses in this `DatabaseInstance` will consult your hook. This is a runtime mutation of the parser registry — which leads directly to the parse-time vs load-time bug in document 02.

## What you can and cannot do from LoadInternal

You are still early in the connection / database lifecycle. You can:
- Register functions and parser extensions.
- Read config.
- Allocate your own static state (see `g_rt` in the brain).

You should **not**:
- Assume a particular `Connection` or transaction exists.
- Start threads that immediately try to run queries against a context you do not yet own (the serve brain does this later, after the scalar is actually called).
- Expect that a `LOAD` inside a `-c` batch has already taken effect for the statements that follow in the same `-c` string.

## Read it yourself

1. `ext-cpp/src/quackapi_extension.cpp:579` — the `DUCKDB_CPP_EXTENSION_ENTRY` block.
2. `ext-cpp/src/quackapi_extension.cpp:510` — `RegisterApplyRoute` and the `LoadInternal` registrations.
3. `ext-cpp/src/quackapi_extension.cpp:560` — `ParserExtension::Register` call.
4. `ext-cpp/CMakeLists.txt:17` — `build_loadable_extension` (this is what actually emits the .duckdb_extension).
5. `ext-cpp/Makefile:1` — delegates to the ci-tools duckdb_extension.Makefile.
6. `ext-cpp/duckdb/extension/demo_capi/` (just look at the directory) — contrast with the C-only example.
7. `edges_round5_draft.md:1` (first three paragraphs) — explicit statement of why the C++ track was required.

## Comprehension questions

1. If you change the DuckDB submodule to a different patch version and rebuild the extension without touching the .cpp files, will `LOAD` succeed against a v1.5.3 `duckdb` binary? Why or why not? Point at the exact mechanism.
2. Why does `ParserExtension::Register` live in `LoadInternal` (called from the entry macro) rather than in a table function or scalar? What would break if you tried to register it lazily on first use of `CREATE ROUTE`?
3. A colleague says "we should ship the C ABI version so users don't have to rebuild for every DuckDB point release." What single feature in the current quackapi design would become impossible? Where in the source is the proof?
4. The tcc-JIT approach and the compiled extension approach both end up with native code calling `duckdb_query`. List two concrete differences visible in the quackapi source tree (one in build, one in what surfaces you can reach).
