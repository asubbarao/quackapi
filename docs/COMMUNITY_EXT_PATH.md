# quackapi as DuckDB Community Extension: Path to `INSTALL duckapi FROM community; LOAD duckapi;`

**Verdict: LEGS-WITH-WORK**

quackapi (the ext-cpp/ C++ component providing `serve_brain`, `quack_route_decision`, ParserExtension `CREATE/DROP ROUTE`, and routing brain) can ship as a community extension. The httpserver precedent (a full HTTP server extension already in community-extensions) proves the category is accepted. Empty vcpkg deps, MIT license, and use of the official extension-template CI lower the bar. Partial platform support via `excluded_platforms` is explicitly supported. However, the submission is not turnkey: the test suite is still the waddle template placeholder (will fail CI), workflow files and extension name references are inconsistent, the source tree lives under `ext-cpp/` (not repo root), Windows support is absent (raw POSIX sockets/pthreads), and ParserExtension + custom dlsym symbol resolution introduce maintenance surface on every DuckDB release. With targeted cleanup + a decision on platform scope (exclude win/wasm or port), a description.yml PR is viable. No GPL, signing is automatic (no `-unsigned`), and the review bar appears CI+basic sanity rather than deep security audit.

## 1. SUBMISSION MECHANICS

Process (cited directly from official sources):

- Community Extensions must be public, open-source, GitHub-hosted.
- Open a PR to https://github.com/duckdb/community-extensions adding exactly one file: `extensions/<name>/description.yml` (directory name must == `extension.name`).
- The YAML drives everything: CI build/test (via `scripts/build.py`), docs generation, and distribution.
- On PR (or push to main of community repo), CI parses the yml, checks out the `repo.github` at `repo.ref`, then delegates to `duckdb/extension-ci-tools/.github/workflows/_extension_distribution.yml` (with `override_repository` + `override_ref`) + code quality.
- If tests pass and maintainers approve, binaries are built for the matrix, signed by DuckDB team, and published to community-extensions.duckdb.org.
- Users then: `INSTALL <name> FROM community; LOAD <name>;` (signed, no `-unsigned`).
- Re-releases: any change to the description.yml (e.g. new `ref`) triggers a rebuild for current stable DuckDB. On each new DuckDB stable release, community-extensions auto-rebuilds all listed extensions against the new version (maintainers can pre-stage via `ref_next` for the codename branch).
- See:
  - https://duckdb.org/community_extensions/documentation.html (exact YAML fields, PR instruction)
  - https://duckdb.org/2024/07/05/community-extensions.html (developer flow, 3 steps)
  - https://raw.githubusercontent.com/duckdb/community-extensions/main/.github/workflows/build.yml (actual pipeline: prepare parses yml, calls distribution with overrides, deploy on main)
  - https://raw.githubusercontent.com/duckdb/community-extensions/main/scripts/build.py (parses `extension.*`, `repo.{github,ref,ref_next,...}`, `excluded_platforms`, etc.; dir name validation)
  - https://raw.githubusercontent.com/duckdb/community-extensions/main/UPDATING.md (release cycle, how DuckDB releases trigger mass rebuilds, `ref` vs `ref_next`)
  - https://duckdb.org/community_extensions/development.html (toolchain alignment with extension-template)

Example working descriptor (httpserver): https://raw.githubusercontent.com/duckdb/community-extensions/main/extensions/httpserver/description.yml

Our source audit (no description.yml yet):
- ext-cpp/ follows the template layout at the *subdir* level (CMakeLists.txt, extension_config.cmake, src/, test/sql/, vcpkg.json, .github/workflows/MainDistributionPipeline.yml, LICENSE, duckdb/ + extension-ci-tools/ submodules).
- No top-level description.yml or equivalent in the quackapi repo root.

## 2. BUILD MATRIX REALITY

Community CI builds (from distribution_matrix.json + build.yml + docs):

Platforms attempted by default (https://raw.githubusercontent.com/duckdb/extension-ci-tools/main/config/distribution_matrix.json):
- linux: linux_amd64 (default), linux_arm64; musl variants are opt-in.
- osx: osx_amd64, osx_arm64.
- windows: windows_amd64 (default), windows_arm64 + windows_amd64_mingw (some opt-in).
- wasm: wasm_mvp, wasm_eh, wasm_threads.

Docs confirm: "Extensions are built, signed and distributed for Linux, macOS, Windows, and WebAssembly." (https://duckdb.org/2024/07/05/community-extensions.html)

`excluded_platforms` (semicolon-separated, e.g. `wasm_mvp;wasm_eh;wasm_threads`) and `opt_in_platforms` supported in description.yml (build.py + workflow pass them as `exclude_archs`/`opt_in_archs`).

httpserver precedent (ships today): excludes only the three wasm variants; full linux/osx/windows coverage. See descriptor above.

Our source audit (POSIX-only surface is isolated to the HTTP server; routing/Parser parts are C++ API only):

All raw POSIX/BSD/pthreads/dlsym in one file:
- /Users/aloksubbarao/quackapi/ext-cpp/src/quackapi_brain.cpp:12-17 (includes):
  - `<sys/socket.h>`, `<netinet/in.h>`, `<pthread.h>`, `<signal.h>`, `<dlfcn.h>`, `<unistd.h>`
- Calls (enumerated via grep + read):
  - socket(2), setsockopt, bind, listen, accept (lines ~510-517)
  - read, write, close (hundreds of times in worker/respond paths, e.g. 213, 226, 374, 380, 391...)
  - usleep (253, 573)
  - pthread_create, pthread_detach, pthread_mutex_lock/unlock/init, pthread_cond_wait/signal/init (205-206, 479-500, 558-569)
  - signal(SIGPIPE, SIG_IGN) (509)
  - dlsym + dlopen (multiple fallbacks for RTLD_DEFAULT, libduckdb.dylib/so, hard-coded /opt/homebrew paths etc.) (132-170, resolve_sym)
- Also: resolve_sym uses RTLD_DEFAULT (-2) + dlopen(0,2) etc.

Classification:
- (a) portable as-is: none of the serve_brain/accept_loop/block_forever path. The pure routing (`quack_route_decision`, `quack_init_router`, registry structs) + DDL ParserExtension live in quackapi_extension.cpp and are portable.
- (b) needs #ifdef _WIN32 shim: everything above (winsock2.h + WSAStartup, CreateThread/_beginthreadex or std::thread, CRITICAL_SECTION or std::mutex, Sleep, GetProcAddress + LoadLibrary for "duckdb symbols", different signal handling or no-op, _close etc.). dlsym fallbacks would also need win equivalents.
- (c) can be excluded on that platform: yes. `serve_brain` / `block_forever` can be made unavailable (or the whole extension excluded). Partial support precedent exists (httpserver excludes wasm; many exts exclude musl or specific wasm).

Partial-platform is acceptable: the extension can still LOAD and provide non-server surfaces (route decision, CREATE ROUTE parser) on excluded platforms if the build produces a binary, but the practical effect of `excluded_platforms` is that no binary is produced for that arch — INSTALL fails for users on excluded platforms. Windows users would be unable to `INSTALL quackapi FROM community` at all if we exclude windows_*.

No other POSIX leakage found outside brain.cpp (extension.cpp, headers, CMake use only DuckDB headers + std).

## 3. TEMPLATE COMPLIANCE

Official expectation (from extension-template + community docs + CI):
- Uses the batteries-included extension-template (CMake + build_static_extension/build_loadable_extension).
- `extension_config.cmake` declaring the extension.
- `test/sql/*.test` files using sqllogictest format; `make test` (or the distribution pipeline) runs them. CI requires them to pass.
- LICENSE, vcpkg.json (even if empty), proper .github/workflows/MainDistributionPipeline.yml + code-quality that reference the correct extension_name.
- Submodules: duckdb + extension-ci-tools pinned appropriately.
- On rename/bootstrap, all references updated.

Our audit (ext-cpp/ is a post-bootstrap tree based on v1.5.x template):
- extension_config.cmake: present and correct for `quackapi` (ext-cpp/extension_config.cmake:3).
- CMakeLists.txt: present, sets TARGET_NAME quackapi, includes both .cpp sources, calls the build_ helpers (ext-cpp/CMakeLists.txt:4-21).
- test/sql/: only waddle.test (template placeholder). It does `require waddle` + tests `waddle('Sam')` and an openssl func that doesn't exist here. Will fail under CI sqllogictest. Real surfaces (`serve_brain`, `quack_route_decision`, `quack_apply_route`, ParserExtension CREATE/DROP ROUTE) have zero .test coverage. Shell harnesses in test/ and probes/ are outside sqllogictest.
- .github/workflows/MainDistributionPipeline.yml: present but wrong: `extension_name: waddle` (twice) and pins v1.5.4 (ext-cpp/.github/workflows/MainDistributionPipeline.yml:21,29). Code-quality same issue.
- Other template files (Makefile, scripts/bootstrap-template.py still present, docs/README.md still says "waddle") show incomplete cleanup after rename.
- No sqllogictests for the core value (HTTP, routing-in-C, ParserExtension DDL). CI will not pass as-is.
- extension-ci-tools/ and duckdb/ submodules are present (required for local build/CI).

Missing before PR: real passing .test files (S/M), fix all "waddle" references in workflows + test (S), ensure `make test` works cleanly in the tree that will be checked out.

## 4. DEPENDENCIES

- vcpkg.json (ext-cpp/vcpkg.json): `{"dependencies": [], ...}` with only overlay config for ci-tools. Zero extra packages. No OpenSSL, no third-party libs.
- This matches the "no dependency" path documented in extension-template README.
- No GPL contamination risk (empty manifest + our code is all original + DuckDB headers under their license).
- LICENSE: present at ext-cpp/LICENSE (MIT, DuckDB Foundation copyright, identical to template). Matches httpserver (MIT).
- No vcpkg ports or custom ports needed beyond the template's.

Cited: https://github.com/duckdb/extension-template (vcpkg section), our vcpkg.json:1-10.

## 5. NAMING

Neither `duckapi` nor `quackapi` appears in the current list of community extensions (https://duckdb.org/community_extensions/list_of_extensions.html or the extensions/ tree).

- httpserver uses the straightforward name "httpserver".
- Other examples: ducktinycc, airport, parser_tools, etc. Lowercase letters/numbers/-/_ only (case-insensitive).
- DuckDB guidance (from docs): "Extension names are case-insensitive, so only lowercase letters, numbers and - or _ are allowed".
- The dream string uses `duckapi`; the compiled extension binary + code uses `quackapi` (QuackapiExtension::Name(), TARGET_NAME, etc.). Choose one consistently; "quackapi" aligns with the repo and current symbols. No conflicts found in search of community list or issues.

## 6. SIGNING/UNSIGNED

Confirmed: community extensions are built + signed by the DuckDB team / community CI infrastructure.

- "All core and community extensions are signed by the DuckDB team." (https://duckdb.org/docs/current/extensions/extension_distribution.html#extensions-signing)
- "The Community Extensions repository performs the steps required for publishing extensions, including building the extensions for all relevant platforms, signing the extension binaries..." (https://duckdb.org/2024/07/05/community-extensions.html)
- Result: `INSTALL foo FROM community; LOAD foo;` works out of the box for users (no `-unsigned`, no `allow_unsigned_extensions`). This is a major UX win vs today's local unsigned builds from ext-cpp/.
- Contrast with custom repo or direct .duckdb_extension load, which require unsigned.

## 7. SECURITY/REVIEW BAR

From sources:
- DuckDB takes over build + sign + distribution (step up from arbitrary pip/npm binaries).
- "a step down from reviewing everything manually" (Securing extensions doc).
- No public evidence of deep manual code review for every submission; focus is on CI passing the same checks as extension-template (format, tidy, sqllogictest, build matrix).
- Network-listening precedent exists and ships: httpserver (starts server on arbitrary host:port, Basic/X-Token auth, play UI), cronjob, http_* family, etc.
- Security docs recommend `SET allow_community_extensions = false;` for locked-down use; users are pointed to source repo for issues.
- Rejections: no prominent public "rejected because network server" stories in searches; the bar appears practical (does it build cleanly? tests pass? name unique? license ok?).
- Precedents of ParserExtension use in community (duckpgq, prql, parser_tools, and at least one crawler extension) show custom parser syntax is tolerated.

Strengtheners for our case (to make PR smooth):
- Default `serve_brain` to localhost (or document loudly).
- Add hello_world + extended_description in description.yml with security notes (e.g. "pair with DuckDB -readonly", "no built-in auth yet").
- Comprehensive sqllogictests (non-listening paths at minimum; perhaps skip server tests or use a test_config).
- Clean README/docs in the extension tree.
- Exclude wasm (and decide on windows) explicitly.

Citations: https://duckdb.org/docs/current/operations_manual/securing_duckdb/securing_extensions.html , community build logs and httpserver acceptance.

## 8. MAINTENANCE COST

- On every DuckDB stable release: community auto-rebuilds from the pinned `ref`. If your tree (at that ref) builds against the new duckdb submodule + ci-tools, you get a free drop-in.
- If incompatible (common for C++ extensions because "DuckDB extensions ... are built against the internal C++ API of DuckDB. This API is not guaranteed to be stable." — ext-cpp/docs/UPDATING.md:13), you must:
  - Create a `vx.y-codename` branch, pin submodules + workflow to the RC, fix, then PR a `ref_next` update into community description.
  - Or post-release, bump on main and PR new ref.
- Specific risk for quackapi: heavy use of `ParserExtension` (quackapi_extension.cpp:328 (class), 559 (Register), RouteDdlParse/Plan, hand parser for CREATE/DROP ROUTE). Also direct use of many internal headers (ClientContext, ExtensionLoader, TableFunction, etc.).
- Template guidance: watch DuckDB release notes, core extension patches (https://github.com/duckdb/duckdb/commits/main/.github/patches/extensions), and header git history.
- httpserver and other server extensions have survived multiple releases, so the category is maintainable.
- Our dlsym symbol hacks + pthread pool + custom registry reload (quack_reload_router) add platform + re-entrancy fragility on updates.
- Cost: low for pure-SQL or simple scalar exts; medium-high here due to Parser + C server. Expect 1-2 weeks of porting work per major DuckDB release in the worst case (historical pattern for complex out-of-tree exts).

Citations: ext-cpp/docs/UPDATING.md (full), community-extensions/UPDATING.md, extension-template docs/README.md:164.

## Concrete Gap Checklist (before a submission PR)

Ordered by dependency (do earlier first). Effort is S/M/L relative to current state.

1. (S) Rename/fix all "waddle" references: MainDistributionPipeline.yml (extension_name + ci pins), test/sql/waddle.test filename + content, any other bootstrap leftovers. Update to quackapi.
2. (S) Add or expand test/sql/ with real sqllogictest cases. At minimum: scalar funcs (quack_route_decision, quack_init_router), quack_apply_route, CREATE/DROP ROUTE syntax + side effects on routes table, error cases. Server tests can be minimal or conditional (serve_brain returns status without actually listening long-term in CI).
3. (M) Decide + implement platform story: either (a) add `excluded_platforms: wasm_mvp;wasm_eh;wasm_threads;windows_amd64;...` (and document "Unix-like only for serve_brain") or (b) implement Windows shims (#ifdef, winsock, threads) so we don't drop an entire major platform. Test the chosen matrix locally where possible without running servers.
4. (M) Ensure the git ref used in description.yml checks out a tree whose *root* matches the template (CMakeLists.txt + extension_config.cmake + src/ at top level). Current layout has everything under ext-cpp/. Options: publish a separate "quackapi-extension" repo that is a clean template clone, move files, or restructure; community build does not appear to support arbitrary SOURCE_DIR for the override case.
5. (S) Create the PR descriptor: `extensions/quackapi/description.yml` with:
   - name: quackapi (or duckapi — decide once)
   - description, version (e.g. date-based or semver), language: C++, build: cmake, license: MIT
   - maintainers: [...]
   - repo: { github: <owner>/quackapi, ref: <clean tag or commit> }
   - excluded_platforms: ...
   - docs: { hello_world: "...", extended_description: "..." } (include security/readonly guidance)
6. (S) Verify `make test` (sqllogictest) + code quality pass against the pinned ci_tools_version in the tree that will be referenced.
7. (M) Add basic hardening docs + perhaps localhost default in serve_brain if not already (or expose clearly). Consider a test_config in yml if special setup needed.
8. (L optional but recommended) Port or stub serve_brain on Windows so the extension is installable everywhere (even if serve is a no-op or errors gracefully).

After the above, open PR to duckdb/community-extensions. Expect iteration on CI results.

## Honest Risks (top 2-3)

1. **ParserExtension + internal C++ API churn (maintenance pain + potential breakage)**: We rely on ParserExtension (registered at Load time, custom parse/plan for DDL) and many ClientContext/ExtensionLoader details. Every DuckDB release requires a porting cycle (documented as non-stable in both template and community UPDATING.md). Unlike table-function-only exts, this is closer to duckpgq-level surface area. If DuckDB moves to PEG or changes registration, the CREATE ROUTE syntax may need full rewrite.

2. **Windows (and to a lesser extent wasm) platform gap**: Raw BSD sockets + pthreads + dlsym + signal is Unix-only. httpserver precedent required (presumably) cross-platform work or careful exclusion. Excluding windows means no `INSTALL` for the huge Windows DuckDB user base (CLI, Python, etc.). Porting the server loop is non-trivial (winsock, threading model, symbol resolution across python/R clients). Partial "loads but serve_brain unavailable" may be confusing if we still ship a windows binary.

3. **Source layout + test debt delaying first successful CI**: The community build clones the ref and expects a root-level template tree + passing sqllogictests. Current state (waddle tests, wrong workflow names, server code under ext-cpp/, no real .test coverage for the Parser or brain) will produce red CI on the submission PR. Combined with the need to possibly publish the extension sources from a differently-structured repo, this is the most likely source of "why is my PR not merging" friction. The httpserver authors had a clean dedicated repo.

All other items (deps, signing, basic naming, httpserver category precedent) are in good shape and lower risk.

**Next concrete step after this doc**: create a clean tag on a tree that satisfies checklist items 1-3 + 6, draft the description.yml locally, and simulate (or let the community PR CI do) the build. No servers were started and no other files modified for this discovery.