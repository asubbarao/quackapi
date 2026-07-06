# quackapi C Extension API Discovery Report

**Date:** 2026-07-01  
**Agent:** DISCOVERY (pure research, no implementation)  
**Sources read first (via shell):**  
- `/Users/aloksubbarao/quackapi/README.md`  
- `/Users/aloksubbarao/quackapi/edges.md` (esp. edge #9 Round 3)  
- `/Users/aloksubbarao/quackapi/framework.sql` (`handle_request` TABLE macro)  
- `/Users/aloksubbarao/quackapi/serve_brain.sql` (ducktinycc Tier-2 server)

## Context (from ground truth)
quackapi implements a FastAPI/Pydantic equivalent **entirely** as data + a single self-contained `handle_request(method, path, headers, body)` TABLE macro (routing by segment arrays + QUALIFY, validation via TRY_CAST + param_schema constraints + error aggregation into FastAPI-shaped JSON, OpenAPI as SELECT, handler templating via list_reduce/replace, static bodies and SSE in the same pipeline).

**Pure track (current):** SQL brain + thin C accept loop (via ducktinycc JIT). Routing/validation/serialization = one OLAP query per request. Measured floor after all honest optimizations (no derived cache tables): **~1k req/s flat**, even on `/health`. The 34k+ numbers come from trivial single-table point queries (`/q2`) or static responses.

**Proper-C track (target):** identical high-level contract, but move the *match + validation loop* into ahead-of-time compiled C (clang -O3). Registry tables (`routes` + `param_schema`) stay as SQL data. The C server does cheap C-side route lookup / param binding, then DuckDB executes *only* the rendered handler (or serves a pre-rendered body). Goal = uvicorn-class throughput on the same hardware where a single-table point query already does 34k.

Current Tier-2 (`serve_brain`): 16-worker pthread accept loop + per-worker persistent connection + prepared `"SELECT * FROM handle_request(?, ?, ?, ?)"`. It does *not* contain routing; it calls the macro. It obtains duckdb_* symbols exclusively via `dlsym(RTLD_DEFAULT)` (plus dlopen fallbacks) because it is JIT-compiled by TinyCC *inside* the running process.

## 1. Can a pure-C compiled `.duckdb_extension` do this?

**Yes.**

The stable C Extension API (experimental template + `duckdb_ext_api_v1` vtable) lets you:

- At `LOAD` time (ENTRYPOINT), receive a `duckdb_connection` and register scalar (or table) functions using `duckdb_create_scalar_function` / `duckdb_scalar_function_*` / `duckdb_register_scalar_function`.
- Implement a `serve_brain(port, db_path)` (or similar) whose body, when first invoked, spawns pthreads that run an accept loop + worker pool.
- Each worker does exactly what the current serve_brain C does: `duckdb_connect` (or open+connect), `duckdb_prepare`, `duckdb_bind_varchar` (for the four args), `duckdb_execute_prepared`, row extraction via `duckdb_value_*` / `duckdb_row_count`, `duckdb_destroy_*`, plus SSE chunked write loops.
- The **exact same** low-level socket + pthread + queue + HTTP parse/respond source from `serve_brain.sql` (the big tcc_module string) can be moved into `.c` files with almost no change.
- **Key mechanical change vs ducktinycc**: remove the entire `resolve_sym` / `dlsym(RTLD_DEFAULT)` + dlopen fallback forest + hundreds of typedefs for duckdb_* function pointers. After the loader initializes the vtable, you simply call `duckdb_prepare(...)`, `duckdb_execute_prepared(...)` etc. directly. The header provides:

  ```c
  #define duckdb_prepare   duckdb_ext_api.duckdb_prepare
  ...
  ```

  (See the full mapping block in the generated `duckdb_extension.h`.)

The extension can be marked volatile (per-call) or stable as appropriate. Background "just runs" behavior is achieved the same way as today: pthread_create the accept_loop (or a detached controller thread) from inside the scalar function; the SELECT returns immediately with "BRAIN_LISTENING".

The ENTRYPOINT signature (from template and header):

```c
DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info, struct duckdb_extension_access *access) {
    // register functions here using the provided connection
    return true;
}
```

Concrete registration pattern (from `capi_quack.c` in the template + add_numbers.c):

```c
duckdb_scalar_function fn = duckdb_create_scalar_function();
duckdb_scalar_function_set_name(fn, "serve_brain");
... add parameters (int32 + varchar), set return varchar, set volatile ...
duckdb_scalar_function_set_function(fn, serve_brain_impl);
duckdb_register_scalar_function(connection, fn);
duckdb_destroy_scalar_function(&fn);
```

The impl receives `duckdb_function_info`, `duckdb_data_chunk input`, `duckdb_vector output`. Extract the two args (port + path) from the chunk vectors on the first row (or treat as control call).

**Reusing the current C source**: the sockaddr structs, socket/bind/listen/accept/read/write, pthread queue, worker_main that does the handle_conn logic, the streaming vs non-streaming branches — all move verbatim into a `.c` that is compiled with the host C compiler instead of tcc. Only the symbol acquisition block disappears.

Real URLs:
- Template: https://github.com/duckdb/extension-template-c (and its README)
- Reference implementation + stability discussion: https://github.com/duckdb/reference-extension-c
- C API client docs (the prepare/execute symbols live here too): https://duckdb.org/docs/lts/clients/c/overview.html and https://duckdb.org/docs/lts/clients/c/api.html
- Community example already published: `capi_quack`

## 2. Build toolchain: extension-ci-tools / Makefile, vcpkg, pure-C?

**Lightweight for pure C.**

The C template (`extension-template-c`) is deliberately different from the main C++ `extension-template`:

- `CMakeLists.txt`: `project(... LANGUAGES C)`, `add_library(SHARED ...)` over `.c` files only. No C++ required.
- `Makefile` (top level): tiny wrapper that does `include extension-ci-tools/makefiles/c_api_extensions/base.Makefile` + `c_cpp.Makefile`, then `make configure && make release`.
- `make configure` creates a Python venv (duckdb + sqllogictest) + writes platform/version files. No vcpkg bootstrap.
- Actual compilation is standard CMake + C compiler (clang/gcc). Optional Ninja/ccache for speed.
- The heavy lifting for cross-platform matrix lives in `extension-ci-tools` (docker images, GH workflows that invoke the makefiles, metadata append script that turns the .so/.dylib/.dll into a `.duckdb_extension` by appending the binary footer + version/platform info).

**vcpkg?** Not in the C template path for a pure-libc extension. The main C++ template + many community extensions use vcpkg for third-party C++ deps. A quackapi extension doing only sockets + pthreads + the DuckDB C API symbols needs **zero** external packages.

**Heaviness**: comparable to other community extensions. Requires Python + CMake + make on the dev machine. The CI (when published) does the real multi-arch builds. "No DuckDB build required" (you do not compile the engine; you link against the C API headers at extension compile time).

Real URLs / files fetched:
- https://raw.githubusercontent.com/duckdb/extension-template-c/main/Makefile
- https://raw.githubusercontent.com/duckdb/extension-template-c/main/README.md
- https://raw.githubusercontent.com/duckdb/extension-ci-tools/main/makefiles/c_api_extensions/base.Makefile
- https://raw.githubusercontent.com/duckdb/extension-template-c/main/CMakeLists.txt
- https://raw.githubusercontent.com/duckdb/extension-template-c/main/src/capi_quack.c (and add_numbers.c)
- Community dev page: https://duckdb.org/community_extensions/development.html

## 3. Distribution: community-extensions, per-platform, INSTALL/LOAD

**Standard community flow (already exercised by capi_quack).**

1. In your repo you keep the template layout (or a close derivative): `CMakeLists.txt`, `Makefile` that includes the c_api_extensions makefiles, `src/*.c`, `duckdb_capi/` (submodule or copied headers), `test/sql/`.
2. Cut a commit (or tag).
3. Open a PR to https://github.com/duckdb/community-extensions adding `extensions/quackapi/description.yml`:

   ```yaml
   extension:
     name: quackapi
     description: ...
     version: 0.1.0
     language: C/C++
     build: CMake
     license: MIT
     requires_toolchains: "python3"
     maintainers: [...]
   repo:
     github: your/quackapi
     ref: <commit-sha>
   ```

4. The repo's CI (powered by extension-ci-tools) checks out your ref, runs the configure + release steps for every platform in the distribution matrix, produces the signed/packaged `.duckdb_extension` artifacts, and makes them available under the central community extensions host.

5. Users (any stock DuckDB):

   ```sql
   INSTALL quackapi FROM community;
   LOAD quackapi;
   SELECT serve_brain(18080, 'myapp.db');
   ```

The C template README currently says "(Coming soon) Works with community extensions", but `capi_quack` is already listed and has a working description.yml that points at the template repo. The mechanism is live for C-API extensions.

Real URLs:
- Development page + publishing instructions: https://duckdb.org/community_extensions/development.html
- Live example descriptor: https://raw.githubusercontent.com/duckdb/community-extensions/main/extensions/capi_quack/description.yml
- List: https://duckdb.org/community_extensions/list_of_extensions.html (contains capi_quack)

## 4. ABI stability

**Yes — the C Extension API is the stable-ABI path.**

- All access is through a single large struct of function pointers (`duckdb_ext_api_v1`).
- "The DuckDB C Extension API works through a large struct of function pointers that can only grow but never be modified." (reference-extension-c)
- Versioning is explicit (`DUCKDB_EXTENSION_API_VERSION_MAJOR/MINOR/PATCH`). Stable portion vs an "unstable" section guarded by a define.
- Consequence: a binary built against the stable v1.2.0 surface is intended to continue working against later DuckDB releases that expose at least that surface. No per-DuckDB-version rebuild required for the common case.
- Contrast with the C++ extension API: "tightly coupled to DuckDB’s internal APIs, so it can (and often will) change between DuckDB versions... requires building the whole DuckDB engine".

This is exactly the property quackapi wants for a "proper" native extension that users can `INSTALL` once and have it keep working.

Citations: https://github.com/duckdb/reference-extension-c (README), https://duckdb.org/2026/03/20/duckdb-extensionkit-csharp.html (excellent summary of stable vs C++), generated header comments in the template.

## 5. Limitations (what a C-API extension cannot do that quackapi might eventually want)

- **Parser / custom SQL syntax**: No. You cannot add new statement forms, new clause syntax, or a `REGISTER_ROUTE` DDL that the SQL parser understands. Route registration will stay "INSERT INTO routes SELECT * FROM register_route(...)" (or the macro) forever on the stable C surface. Parser extensions require deeper internal hooks that live in the C++ API or core.
- **Deep engine hooks** (certain replacement scan / file format / storage extensions may have more surface in C++).
- **Host connection identity without path**: The ENTRYPOINT gives you a `connection`. Creating additional connections for the 16 workers is straightforward via `duckdb_open(path)` (exactly what serve_brain does today) or instance cache APIs (some marked unstable). Sharing the *exact same* in-memory catalog state or attached DB handles across the background threads without re-opening by path may require unstable APIs or passing a database handle obtained from the init connection. For quackapi's current design (file path passed to `serve_brain`), this is a non-issue.
- **Threading / sockets portability**: You write the code. Current quackapi C uses pthreads + BSD `sockaddr_in` layout. A real cross-platform extension needs `#ifdef` or a tiny portability layer for Windows threads and sockaddr. Nothing prevents this; you just have to do it.
- **No sandbox**: Same as ducktinycc — a C bug in your accept loop or responder crashes the whole DuckDB process.
- **Vectorized vs row**: Registration of user-facing functions is vectorized (data chunk / vector). The *internal* workers in the server loop continue to use the classic prepare/execute client API (which is fully exposed). Fine.
- **Wasm / exotic targets**: Sockets and long-lived background threads are constrained or impossible in some Wasm embeddings. The matrix will simply not produce a useful binary for those, or the function will be documented as "native hosts only".

Nothing on the list blocks the core "proper-C track" goal (C does routing+validation; DuckDB runs only the handler; uvicorn-class throughput).

## MINIMAL concrete C skeleton (socket + prepare/execute_prepared on host connection)

This is the smallest self-contained illustration that could live under `src/` in a template-derived layout. It registers `serve_brain(port, db_path)` as a volatile scalar. On first call it starts a trivial background listener thread. A worker accepts one connection, does a prepare/execute against a connection it opens from the supplied path (reusing the exact pattern), and writes a canned response.

```c
// src/quack_serve.c
// Minimal skeleton for a C-API quackapi extension.
// Build with the extension-template-c layout + your real accept/worker code.

#include "duckdb_extension.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int g_started = 0;
static pthread_t g_accept_thread;

typedef struct {
    int port;
    char db_path[1024];
} serve_args;

static void *worker_main(void *arg) {
    // In real code: pool of these, persistent conns, prepared handle_request etc.
    duckdb_database db = NULL;
    duckdb_connection con = NULL;
    // Use the exact same open/connect as serve_brain today.
    if (duckdb_open(((serve_args*)arg)->db_path, &db) != DuckDBSuccess) return NULL;
    if (duckdb_connect(db, &con) != DuckDBSuccess) { duckdb_close(&db); return NULL; }

    // Demo: execute a trivial prepared statement exactly as the real workers will.
    duckdb_prepared_statement stmt = NULL;
    if (duckdb_prepare(con, "SELECT 42 AS x", &stmt) == DuckDBSuccess) {
        duckdb_result res;
        if (duckdb_execute_prepared(stmt, &res) == DuckDBSuccess) {
            // In real code: extract status/body/handler_sql columns via value_varchar etc.
            // Here we just demonstrate the call succeeded.
            duckdb_destroy_result(&res);
        }
        duckdb_destroy_prepare(&stmt);
    }
    duckdb_disconnect(&con);
    duckdb_close(&db);
    return NULL;
}

static void *accept_loop(void *arg) {
    serve_args *a = (serve_args*)arg;
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return NULL;
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(a->port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(listen_fd); return NULL; }
    listen(listen_fd, 128);

    for (;;) {
        int client = accept(listen_fd, NULL, NULL);
        if (client < 0) continue;
        // In real code: enqueue to worker pool. Here we just demo one worker.
        pthread_t w;
        pthread_create(&w, NULL, worker_main, a);
        pthread_detach(w);
        close(client); // placeholder; real responder would keep fd
    }
    close(listen_fd);
    return NULL;
}

// Scalar function implementation (called when user does SELECT serve_brain(...))
static void serve_brain_fn(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
    idx_t n = duckdb_data_chunk_get_size(input);
    if (n == 0) return;

    // Extract args (port int, path varchar). Production code uses proper getters + validity.
    duckdb_vector port_vec = duckdb_data_chunk_get_vector(input, 0);
    duckdb_vector path_vec = duckdb_data_chunk_get_vector(input, 1);
    int32_t *ports = (int32_t*)duckdb_vector_get_data(port_vec);
    // For brevity we assume non-null first row and copy path...
    char *path_ptr = duckdb_get_varchar(duckdb_create_varchar("")); // placeholder; use vector + length in real

    static serve_args args;
    args.port = ports[0];
    strncpy(args.db_path, "/tmp/quackapi_bench.db", sizeof(args.db_path)-1); // in real: from path_vec

    if (!g_started) {
        g_started = 1;
        pthread_create(&g_accept_thread, NULL, accept_loop, &args);
        pthread_detach(g_accept_thread);
    }

    // Return a status string (the real version returns "BRAIN_LISTENING pool=16" or error)
    duckdb_vector_assign_string_element(output, 0, "BRAIN_LISTENING (c-api skeleton)");
}

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info, struct duckdb_extension_access *access) {
    duckdb_scalar_function fn = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(fn, "serve_brain");

    duckdb_logical_type i32 = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
    duckdb_logical_type vc  = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    duckdb_scalar_function_add_parameter(fn, i32);
    duckdb_scalar_function_add_parameter(fn, vc);
    duckdb_scalar_function_set_return_type(fn, vc);
    duckdb_destroy_logical_type(&i32);
    duckdb_destroy_logical_type(&vc);

    duckdb_scalar_function_set_volatile(fn); // important: side effects + background thread
    duckdb_scalar_function_set_function(fn, serve_brain_fn);

    duckdb_register_scalar_function(connection, fn);
    duckdb_destroy_scalar_function(&fn);

    return true;
}
```

To turn the above into a loadable extension you would:

1. Drop it (plus any supporting .c for the real HTTP + worker pool + full handle_conn logic) into the template's `src/`.
2. Update `CMakeLists.txt` `EXTENSION_SOURCES`.
3. Keep or adapt the top-level `Makefile` that pulls in the c_api_extensions makefiles.
4. Add `test/sql/` sqllogictest cases that do `LOAD '...'; SELECT serve_brain(0, ':memory:');` (or a real port).
5. `make configure && make release` produces `build/release/quackapi.duckdb_extension`.

The worker code can be a near line-for-line lift of the inner parts of the current `serve_brain.sql` tcc source, minus the symbol resolution.

## VERDICT

The DuckDB C Extension API is sufficient for quackapi's proper-C track and is the right vehicle. It replaces the runtime tcc + dlsym indirection with ahead-of-time clang -O3 compilation against a stable, grow-only function-pointer vtable, lets you register a `serve_brain(port, db)` (or equivalent) that re-uses essentially the entire existing accept-loop C source, moves routing+validation into native C while the registry stays in ordinary SQL tables, and leaves DuckDB executing only the final rendered handler (the part that already clocks 34k req/s). The concrete costs are modest and well-understood: adopt the (experimental but shipping) C template + its CMake/metadata packaging, accept per-platform CI builds (already handled by the community-extensions machinery and demonstrated by `capi_quack`), write a thin portability shim for threads/sockets if you want Windows, and live with the fact that custom SQL parser syntax for routes is out of scope (the existing data-driven `routes` + `register_route` surface is the intended ergonomic path). One binary can target multiple DuckDB versions; distribution is the standard `INSTALL ... FROM community; LOAD ...` flow; the only thing pure SQL genuinely could not cross (the per-request OLAP-router tax) becomes crossable. This is the crossing the edge ledger was waiting for.