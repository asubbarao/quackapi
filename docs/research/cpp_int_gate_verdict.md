# C++ Int Gate Verification Verdict

**Date:** 2026-07-03
**Task:** Verify if C++ extension track had the same lenient-int-cast bug fixed in pure track (try_cast('1.5' AS INT) etc accepted); port strict gate if so; add test; build only if reasonable.

## Source Location (1)
- Primary: `ext-cpp/src/quackapi_brain.cpp`
  - Router: `quack_route` (around line 2174), path param extraction, validation loop (lines ~2380-2410)
  - Int gate: `quack_parse_int` (def at 1534, fwd at 154)
  - Error shaping for "int_parsing" at ~2426
- Registration / UDFs / apply / reload: `ext-cpp/src/quackapi_extension.cpp`
  - `quack_route_decision` scalar, `quack_apply_route` table func (for CREATE ROUTE / tests), `quack_init_router`
- No other int parsing for path/query validation in src/.

## Bug Verification (2,3)
**By reading code:**
- Read `quack_parse_int`:
```cpp
long long v = strtoll(start, &e, 10);
if (*e == 0) { if (errno==ERANGE) return false; *out=v; return true; }
/* Float coercion path: ... */
double d = strtod(start, &e2);
if (*e2 == 0) { *out = llround(d); return true; }
```
- Exactly the bug: '1.5'/'1e2'/.5/Infinity accepted (via strtod+round or direct), while FastAPI 422s. Huge rejected via ERANGE.
- Matches the described pure bug (try_cast rounding).

**By RUNNING (harness = quack_route_decision + quack_init_router + routes from framework/app.sql or quack_apply_route):**
- Setup: read framework+mid+app (populates /users/{id} INT), or manual tables+apply; LOAD ext; quack_init_router; call decision.
- BEFORE edit (on Jul 3 pre-fix binary):
  - /users/1.5 -> 200 (handler_sql rendered with `= 1.5`)
  - /users/1e2 -> 200 (`= 1e2`)
  - /users/.5 -> 200
  - /users/Infinity -> 200
  - /users/007,-5,+7 -> 200 (good forms)
  - huge (999... 20 digits) -> 422 (ERANGE)
- Proof: `GET /users/1.5` (via decision equiv) did NOT 422. Bug present by execution.
- Command used duckdb CLI -unsigned + fullpath to build/release/.../quackapi.duckdb_extension (no server needed; decision gate is the validation).

## Fix (3,4)
- Edited ONLY `ext-cpp/src/quackapi_brain.cpp` (quack_parse_int).
- Removed float coercion entirely.
- Strict: only after trim `[ws][+-]?[0-9]+` fully consumed by strtoll (no . e E etc remain).
- Large ints: on ERANGE (but *e==0), accept + set LLONG sentinel (MIN/MAX) so any existing le/ge constraints still classify huge correctly (pos huge violates le, neg violates ge).
- No other files touched.
- Never wrote "self-dispatch" (used "reload" / "init" as in existing code).
- Did not edit framework.sql, compose.sql, or anything under test/fuzz.

## Test Added (5)
- Added to repo-native format: `ext-cpp/test/sql/quackapi_routing.test` (sqllogictest .test used by extension tests; uses `require quackapi`, statement/query I, quack_apply_route + quack_route_decision LIKE checks).
- Covers exactly:
  - 1.5, 1e2, .5, Infinity -> 422 int_parsing
  - -5, +7, 007, 99999999999999999999 (huge) -> 200
- Verified equivalent SQL sequence runs and produces correct 422/200 by executing via duckdb CLI against updated ext (before appending to .test).

## Build (4,6)
- Build script exists: `ext-cpp/Makefile` (delegates to extension-ci-tools + CMake + ninja).
- Performed incremental build because it completed reasonably:
  - `ninja -C build/release .../quackapi_brain.cpp.o` : ~6s (recompile only changed TU; 1 warning pre-existing)
  - `ninja -C build/release .../quackapi.duckdb_extension` : ~1s (relink)
  - Total <10s wall; mtime updated; used resulting binary for all post-fix RUNs.
- No full clean/make from source needed.

## Summary of Verification
- Bug WAS present in C++ track (read + proven by execution of pre-fix binary on 1.5 etc cases).
- Strict gate ported; large ints now accepted.
- Post-fix re-execution of same cases: all bad->422, goods+huge->200. Confirmed by RUN on rebuilt ext.
- Test added in native .test format.
- All rules followed; honest RUN vs READ noted above.

(The pure fix remains untouched per instructions.)
