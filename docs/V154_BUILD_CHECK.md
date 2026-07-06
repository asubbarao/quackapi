# DuckDB v1.5.4 Build & Test Check for quackapi Extension

**Date:** 2026-07-03  
**Workspace isolation:** All work performed exclusively in `/Users/aloksubbarao/quackapi/ext-cpp-154-check/` (rsync copy) + this report file only.  
**Original tree untouched:** `/Users/aloksubbarao/quackapi/ext-cpp/` was never modified (confirmed via rsync excludes + no commands targeting it). No .sql files outside the copy were edited.  
**Safety compliance:**  
- No operations on ports 9494/9495.  
- Smoke test used pure in-memory queries (no servers started).  
- No `pkill`, `killall`, or broad kills; only copy-specific processes via the build itself.  
- All commands used exact paths/PIDs where relevant; no side effects.

## Summary

- **Builds against v1.5.4:** YES  
- **Tests pass:** YES (56 assertions in 3 test cases)  
- **API breakage found:** NONE  
- **Mechanical fixes applied:** NONE (0 lines changed)  
- **Smoke test (CREATE ROUTE + quack_route_decision):** PASSED  
- **Verdict:** Safe to ship against DuckDB v1.5.4. The current CI pin at v1.5.4 is compatible; no regressions vs the submodule's prior v1.5.3 pin. No source or behavior changes required.

## Steps Performed (exact)

1. Isolated copy (no original touch):
   ```
   rsync -a --exclude build --exclude duckdb_unittest_tempdir \
     /Users/aloksubbarao/quackapi/ext-cpp/ \
     /Users/aloksubbarao/quackapi/ext-cpp-154-check/
   ```

2. Submodule update in copy only:
   ```
   cd /Users/aloksubbarao/quackapi/ext-cpp-154-check
   git -C duckdb fetch --depth 1 origin tag v1.5.4
   git -C duckdb checkout v1.5.4
   ```
   Result: `v1.5.4` (commit `08e34c447bae34eaee3723cac61f2878b6bdf787`)

3. Build (vcpkg toolchain, release, logged):
   ```
   export VCPKG_TOOLCHAIN_PATH=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake
   make release > build_154.log 2>&1
   ```
   - Started: ~2026-07-03 20:55 PDT
   - Completed: ~2026-07-03 21:07 PDT (~12 minutes wall time)
   - Full log: `build_154.log` (553 MiB build dir)
   - Key cmake: `git hash 08e34c447b, version v1.5.4, extension folder v1.5.4`
   - Extensions linked: `[quackapi, core_functions, parquet]`
   - No compile failures. (The only "error" grep hit was filename `error_private.cpp.o`.)

4. Gate tests:
   ```
   ./build/release/test/unittest "test/*"
   ```
   Raw output (full `test_154.log`):
   ```
   Filters: test/*

   [0/3] (0%): test/sql/quackapi_route_ddl.test
   [1/3] (33%): test/sql/quackapi_errors.test
   [2/3] (66%): test/sql/quackapi_routing.test
   [3/3] (100%): test/sql/quackapi_routing.test
   ===============================================================================
   All tests passed (56 assertions in 3 test cases)
   ```
   - 3 test files executed (quackapi_route_ddl, quackapi_errors, quackapi_routing).
   - 56 assertions, 0 failures.
   - Exit status: 0.

5. Optional smoke (copy's own shell only):
   ```
   ./build/release/duckdb <<'SMOKE'
   LOAD 'build/release/extension/quackapi/quackapi.duckdb_extension';
   ... (CREATE OR REPLACE TABLE routes + param_schema)
   CREATE ROUTE smoke_test GET '/smoke/{id}' (id INT) AS SELECT {id} AS val, 'ok' AS status;
   SELECT quack_route_decision('GET', '/smoke/123', '{}', '');
   DROP ROUTE smoke_test;
   SELECT quack_route_decision('GET', '/smoke/123', '{}', '');
   SMOKE
   ```
   Results (excerpt):
   - Extension: `quackapi` reported as `(BUILT-IN)` + `loaded=true` (static link in this build; LOAD issued).
   - `CREATE ROUTE ...` → `ROUTE_CREATED`
   - Route count: 1
   - Decision (param substituted):
     ```
     {"status":200,"content_type":"application/json","body":null,"handler_sql":"SELECT 123 AS val, 'ok' AS status","resp_headers":"{}"}
     ```
   - `DROP ROUTE` → `ROUTE_DROPPED`
   - Post-drop decision: `{"status":404,...}`
   - Final: `OK`

   Full capture: `smoke_154.log`

## API / Compatibility Notes

- No DuckDB internal-API churn observed.
- No header/class changes required any porting.
- No compile errors, link errors, or test assertion failures.
- ParserExtension (CREATE ROUTE / DROP ROUTE) and scalar function `quack_route_decision` worked identically to expectations.
- Static build (`EXTENSION_STATIC_BUILD=1`) + loadable artifact both present and functional.
- Submodule bump from v1.5.3 → v1.5.4 was a clean drop-in for this extension.

## Changes Made in This Verification

- **Source code:** 0 lines edited.
- **Exact diff:** (none; `git diff --stat` inside copy only shows build artifacts + logs + untracked test sources that were rsynced in).
- Only generated artifacts: `build_154.log`, `test_154.log`, `smoke_154.log`, and the report.

## Artifacts in ext-cpp-154-check/

- `build/release/duckdb` (43 MiB, v1.5.4)
- `build/release/test/unittest` (43 MiB)
- `build/release/extension/quackapi/quackapi.duckdb_extension` (~27 MiB)
- `build/release/repository/v1.5.4/osx_arm64/quackapi.duckdb_extension`
- Logs as noted.

## Verdict

**Builds: YES**  
**Tests: PASS**  
**Smoke: PASS**  
**Verdict: Ship against v1.5.4.**  

The CI workflow's pin at DuckDB v1.5.4 is validated for the quackapi extension. Current source is fully compatible; no adjustments needed. The isolated tree can be discarded after review.

---
*Report generated by verification run. All commands confined per HARD SAFETY RULES.*

## Addendum (same day, post-fix source)

The check above ran against the tree as of ~20:47, which predated two src changes
landed minutes later (422 `input`-field parity + localhost-default bind /
3-arg `serve_brain`). The updated `src/` + `test/sql/` were then synced into this
copy and rebuilt; result on v1.5.4:

```
All tests passed (59 assertions in 3 test cases)
```

**Verdict unchanged and strengthened: current (fixed) source ships clean on v1.5.4.**

Build-system gotcha recorded twice today: `rsync -a`/editor timestamps older than
existing object files make ninja skip recompilation, and `make test` then runs a
stale statically-linked `unittest` — producing phantom failures (or phantom passes).
When syncing sources into an already-built tree, `touch src/*` before `make`.