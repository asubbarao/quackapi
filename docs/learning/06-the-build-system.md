# 06 — The Build System

`make release` (and its siblings) turns the two .cpp files plus the pinned DuckDB submodule into a loadable `.duckdb_extension`.

This document tells you exactly what happens, why the first build takes 20–40 minutes, why subsequent rebuilds are fast, and how to read the terrifying compiler output when something goes wrong.

## What `make release` actually runs

The top-level Makefile is tiny:

```makefile
# ext-cpp/Makefile:1
PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
EXT_NAME=quackapi
EXT_CONFIG=${PROJ_DIR}extension_config.cmake
include extension-ci-tools/makefiles/duckdb_extension.Makefile
```

It delegates to DuckDB's official extension CI makefile. That makefile drives CMake.

The interesting file for configuration is:

```cmake
# ext-cpp/CMakeLists.txt:1
cmake_minimum_required(VERSION 3.5)
set(TARGET_NAME quackapi)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
set(CMAKE_CXX_STANDARD "17" ...)

include_directories(src/include)
set(EXTENSION_SOURCES src/quackapi_extension.cpp src/quackapi_brain.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})
```

`build_loadable_extension` (defined in DuckDB's extension build helpers) produces the `.duckdb_extension` artifact that you `LOAD`.

## The major steps on a clean `make release`

1. **CMake configure**  
   Runs the `CMakeLists.txt` above. Discovers the DuckDB submodule, sets up the two extension targets (static + loadable), pulls vcpkg manifest if needed.

2. **DuckDB submodule build**  
   The `ext-cpp/duckdb/` directory is a full copy of the DuckDB v1.5.3 tree. The extension build treats it as a subdirectory and builds a lot of it (core, parsers, execution, many extensions). This is the bulk of the first-build time.

3. **vcpkg manifest install** (if ports are declared)  
   `vcpkg.json` in this repo declares almost nothing:

   ```json
   // ext-cpp/vcpkg.json:1
   {
     "dependencies": [],
     "vcpkg-configuration": { "overlay-ports": ["./extension-ci-tools/vcpkg_ports"], ... }
   }
   ```

   So for quackapi the vcpkg step is nearly a no-op (unlike extensions that pull openssl, aws, etc.).

4. **Compile the two sources**  
   `quackapi_extension.cpp` and `quackapi_brain.cpp` are compiled against the DuckDB headers from the submodule + system headers for sockets/pthreads.

5. **Link + metadata**  
   The loadable extension target links against the DuckDB symbols that will be provided at runtime by the host process (no static link of the whole engine into the .so). Extension metadata (version, ABI stamp, platform) is appended so the loader can reject mismatches.

6. **Artifact lands**  
   Look under:

   ```
   ext-cpp/build/release/repository/v1.5.3/osx_arm64/quackapi.duckdb_extension
   ```

   (or the equivalent for your platform). There is also a copy in `build/release/extension/quackapi/quackapi.duckdb_extension` etc.

The build also produces a lot of intermediate `.o` files under `build/release/src/` and under the duckdb build tree.

## Why the first build is glacial and rebuilds are not

The DuckDB submodule is ~hundreds of thousands of lines. On a clean configure + build:

- It configures and builds a large portion of DuckDB as a static library / object set that the extension links against (or uses headers from).
- Many third-party things (fmt, re2, etc.) get built.
- Debug vs release and the various extension build options multiply the work.

After the first successful build:

- CMake / Ninja (the generator used) has a full dependency graph.
- Touching only `src/quackapi_brain.cpp` causes only that translation unit to be recompiled and the extension target relinked.
- The huge DuckDB tree is already built; its objects are up to date.

This is why the docs repeatedly say "the first build takes forever and rebuilds don't."

To force a clean incremental rebuild of just your code:

```bash
cd ext-cpp
make release
# or, inside the build dir for speed
cd build/release
ninja quackapi_loadable_extension   # or whatever the exact target name is
```

To do a truly clean build (you changed DuckDB version, CMake flags, etc.):

```bash
rm -rf build/
make release
```

## Reading a C++ compile error top-down

When you break the build you will see pages of template instantiation backtraces. The rule is simple:

**Read the first error only. Ignore everything until the next "error:" line.**

Example structure you will see:

```
In file included from ...
/path/to/duckdb/src/include/duckdb/main/client_context.hpp:123: error: no matching function for call to ...
   ... 40 lines of template arguments ...
/path/to/your/quackapi_extension.cpp:378:   required from here
   ... more templates ...
ext-cpp/src/quackapi_extension.cpp:378: error: use of deleted function ...
```

The actionable information is almost always:

- The file + line in *your* .cpp (quackapi_extension.cpp:378 in the example).
- The first one or two lines of the actual diagnostic after the "error:" token.

Template spew after that is the compiler explaining *why* the types didn't match. You usually only need the first failing site + the primary message.

Common quackapi-era mistakes that produce walls of text:

- Forgetting `std::move` when returning a `unique_ptr` from a function that returns `unique_ptr`.
- Passing a `ClientContext&` where a `Connection` or a different context was expected.
- Including the wrong DuckDB header (you get "incomplete type" or missing member errors deep in the trace).
- String API mismatch (`string_t` vs `std::string`, `GetString()` vs direct use).

Always recompile after fixing only the first error the compiler reported. The later errors are often cascading from the first.

## Incremental workflow after touching one file

1. Edit `ext-cpp/src/quackapi_brain.cpp`.
2. `cd ext-cpp && make release` (or `ninja` in the build dir).
3. The build system recompiles only the changed .cpp and relinks the two extension targets.
4. Copy or use the artifact from `build/release/repository/.../quackapi.duckdb_extension`.
5. In your DuckDB session: `LOAD '/absolute/path/to/the/new/quackapi.duckdb_extension';`

Because the loader checks the ABI stamp, if you accidentally load an old artifact you built against a different DuckDB tree, you will get a version mismatch error immediately.

## vcpkg and the overlay ports

The `extension-ci-tools/` directory (vendored from DuckDB) contains:
- The makefiles that know how to produce signed/unsigned loadable extensions.
- vcpkg ports and triplets used by many DuckDB extensions.

quackapi's `vcpkg.json` opts into the overlay but declares no dependencies, so the step is fast. If you ever needed a C library (say, a real HTTP parser or a routing library) you would add it here and the build would fetch/build it via vcpkg before compiling your code.

## The two extension targets

```cmake
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})
```

- `quackapi_extension` (static) — used when DuckDB is compiled with the extension baked in.
- `quackapi_loadable_extension` — the `.duckdb_extension` file you `LOAD` at runtime.

For the quackapi portfolio workflow you only care about the loadable one.

## Artifacts and where to look after build

- `ext-cpp/build.log`, `build_b2.log`, `build_b3.log` — historical full logs from the rounds.
- `ext-cpp/build/release/compile_commands.json` — useful for clangd / IDEs.
- `ext-cpp/build/release/repository/v1.5.3/.../quackapi.duckdb_extension` — the one you actually LOAD in testing.

## Read it yourself

1. `ext-cpp/Makefile:1` — the one-line delegation.
2. `ext-cpp/CMakeLists.txt:17` — the two `build_*_extension` calls and `EXTENSION_SOURCES`.
3. `ext-cpp/vcpkg.json:1` — the empty-deps manifest.
4. `ext-cpp/extension-ci-tools/makefiles/duckdb_extension.Makefile` (skim the top 50 lines) — see how it invokes CMake + the packaging steps.
5. `ext-cpp/build/release/repository/v1.5.3/` (after you have built once) — the actual shipped artifact location.
6. Any of the `build*.log` files — search for "error:" to see real first-error lines.

## Comprehension questions

1. You edit only `quackapi_brain.cpp` and run `make release`. Which translation units are recompiled? Which previous build products are reused without work?
2. A colleague runs `LOAD 'quackapi_extension'` from a v1.5.3 `duckdb` binary but the .so was built against the v1.5.2 submodule. What exact check fails, where in the source tree is the metadata written, and where in the running process is the check performed?
3. When the compiler emits 60 lines of instantiation trace ending in an error in your file, which single line should you read first, and what should you do before looking at the second error?
4. Why does the loadable extension not statically link the entire DuckDB engine inside the .so? What would happen at LOAD time if it did?
5. The first build after a `git clean -fdx` or after switching the DuckDB submodule tag takes ~30 minutes on a fast laptop. The second `make release` (no source changes) takes under 10 seconds. Explain the difference in terms of what CMake/Ninja actually executes in each case.
