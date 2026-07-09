# BACKLOG — everything outstanding that is not a roadmap wave

**Date:** 2026-07-05 (evening; full-repo sweep) · **re-audited 2026-07-09** against git log per
STATUS.md rule #2 (shipped items tombstoned in place — numbering kept stable because §3.4/§3.8
are cross-referenced). Companion to `ROADMAP_10M.md`: the roadmap owns
the feature waves (A–E); this doc owns everything else — engineering debt, deferred gates, doc
merges, publish mechanics, and the queue of decisions waiting on Alok. Every item carries
provenance. When an item ships, delete it here and let the result doc carry the history.

## 0. Project law added 2026-07-05: SUGAR-FIRST

**Every user-facing example, test seed, demo, and doc uses the DDL sugar** — `CREATE ROUTE` /
`CREATE AUTH` / `CREATE POLICY` / future `CREATE API FOR TABLE`. Raw `INSERT INTO
routes/policies/...` appears nowhere except inside the macro/parser implementation itself.
Origin: Alok, on seeing a verification script seed via VALUES — "no one is going to use this if
you have to insert values clauses that describe ur routes." Standing audit item: sweep
README/app.sql/docs/tests for raw registry INSERTs and convert (§3.8).

## 1. Specced, not built

| item | spec | notes |
|---|---|---|
| WebSockets on main port | `specs/WS_SPEC.md` | per-connection threads bypass the 16-worker pool; `CREATE ROUTE ... WS`; effort M |
| TLS | `specs/TLS_SPEC.md` | v1 = documented proxy (caddy) — buildable now; v2 = in-process mbedTLS |
| Multipart binary v2 | `specs/MULTIPART_SPEC.md` §6 | v1 text-safe shipped; v2 = base64 path vs streaming parser (decision open, §7) |
| CREATE AUTH/POLICY C-mirror | `specs/CREATE_POLICY_AUTH_SPEC.md` | **SHIPPED end-to-end** — C enforcement live-verified 2026-07-06 (11/11 curl, tier-1 132/132, 124k req/s unpoliced — `ext-cpp/A1_AUTH_RESULT.md`); oracle `handle_request` wiring shipped Phase-0 2026-07-07 (`ec7ade6`/`602450d`, tier-1 147/147, both tracks re-verified). Row kept only as spec pointer |
| CREATE API FOR TABLE | `specs/TABLE_API_SPEC.md` (written 2026-07-05) | pending Claude+Alok review, esp. the routes-materialization decision inside it |
| Admin UI | `specs/ADMIN_UI_SPEC.md` (written 2026-07-05) | pending review, esp. asset-embedding mechanism |
| Sessions + CSRF | `specs/SESSION_CSRF_SPEC.md` (written 2026-07-05) | pending review; constant-time compare flagged must-fix before "secure" claims |

## 2. Named, not specced (roadmap MUST/SHOULD — see ROADMAP_10M §3/§6 for wave placement)

`CREATE SUBSCRIPTION` (change-feed over shipped SSE) · `CREATE STORAGE` (local/S3) · migrations
auto-diff · backup one-liner + durability doc · typed JS/TS SDK glue + CI OpenAPI validation ·
~~`CREATE HEALTH CHECK` · `/metrics` route~~ (SHIPPED — #1907, merged `e520a0c`/`e2520f0`) · `CREATE CRON` · `CREATE JOB QUEUE` · `CREATE WEBHOOK` ·
`IDEMPOTENCY KEY` clause · `CREATE RATE LIMIT` · `CREATE TOKEN BLACKLIST` · response `CACHE`
clause + ETag/304 · route versioning metadata · trace propagation · `CREATE TENANT` · `CREATE
CONNECTION` · SMTP/templated emails · JS-escape-hatch decision · Quack-protocol multi-writer.

## 3. Engineering debt & known bugs

1. **Oracle crash bugs (P1)** — `test/fuzz/oracle_fuzz.test.sql` + `TEST_PLAN.md:58–71`:
   malformed JSON body throws instead of 422; whitespace-only body bypasses NULL guard;
   duplicate query key (`?q=a&q=b`) throws on `map_from_entries` (dedup fix drafted in
   `_qs_to_map`); float→int coercion accepts 1.5 where FastAPI rejects (documented edge).
2. **Conformance divergences** — 87-case suite: after R4 fixes, ~17 BUG-class remain, mostly
   coercion-policy calls awaiting a human ruling (float rounding, int ceiling, bool coercion);
   intentional diffs documented (trailing slash 404-vs-307, HEAD, Allow header contents,
   multipart error code). `edges.md:519–535`, `TEST_PLAN.md:73–82`.
3. ~~Constant-time compare (P1 security)~~ **SHIPPED 2026-07-07** — `_constant_time_str_equals`
   (XOR-fold) landed in Phase-0, mirrored in C, both tracks re-verified; see `docs/STATUS.md`.
4. **Access-log serialization wall (P2)** — `access_log=true` collapses /health c64 108k → ~1.6k
   (~68×) per `CHRONICLE.md` §6 #12 — NOTE: the 2026-07-04 C-layer wave measured 136k WITH
   logging on; reconcile which doc is stale and re-bench before believing either.
5. **Percent-decoding gap (P3)** — query params arrive URL-encoded (`a%40b` literal); fix
   requires touching the whole param surface + tests at once. `framework.sql:1` note.
6. **Parity blindness** — parity byte-compares oracle vs C, so a symmetric regression is
   invisible; only tier-1 asserts absolute behavior. Keep both green always. (R3 lesson.)
7. **Tier-1 invocation landmine** — suite is self-contained; loading `app.sql` first corrupts
   seeds → ~90 false failures. Warning lives in the test header; keep it.
8. **SUGAR-FIRST audit (new)** — convert any raw registry INSERTs in README, app.sql, docs,
   result-doc examples, and test seeds to DDL sugar (§0).
9. ~~Oracle auth enforcement wiring (P1 parity debt)~~ **SHIPPED 2026-07-07** — Phase-0 wired
   authenticate→authorize→claims-bind into oracle `handle_request` (`ec7ade6`/`602450d`),
   tier-1 147/147 incl. auth rows; see `docs/STATUS.md`.
10. **Claims wrap only exists on policied routes** — a handler referencing `claims`/`_ctx`
    on an unpoliced route 500s. Decide semantics (always-wrap with empty claims vs error).
11. **`{...}` handler literals vs `{param}` templating** — parse-time truncation FIXED
    (leading-`AS` scan bug, quackapi_extension.cpp), but a struct-literal key matching a
    param name still substitutes at render. Consider `:param` or `{{param}}` syntax v2.
12. **api_keys plaintext at rest** — v1 stores raw keys (ct-compare at use); spec §API_KEY
    says `HASH 'sha2-256'`. Hash on CREATE + hash-compare on verify.
13. **Policed-route throughput** — per-request SQL HMAC verify: /ok c32 = 3.3k req/s vs
    124k unpoliced. Verified-token→claims LRU (exp-bounded, sig-keyed) or native C HS256.
14. **`LOAD json` soldered everywhere** — static builds don't autoload core json; it is now
    explicitly loaded in framework.sql, serve-boot, and worker_main. Any NEW connection
    surface (pool replicas, admin conns) must do the same — add to the C-mirror checklist.

## 4. Deferred perf gates (thresholds are law — gate before build)

| gate | threshold | status |
|---|---|---|
| set-based request batching | **≥59k req/s** on /search c64 (+20% over B7's 49,091) | next ceiling-breaker; microbench first (`B7_RESULT.md`) |
| prepared statements | (failed: +2.5–4.6%) | KILLED by B4 gate — do not revisit without new evidence |
| settings/knob sweep | (failed: ≤3%) | KILLED by B5 gate — allocator+pool (B6/B7) was the real answer |
| async/buffered access logging | only if #3.4 re-bench shows a real regression | conditional |

Known honest ceiling: DuckDB per-query task setup tax at sub-ms query scale (~4.8× shared / ~6.1×
separate instances of a ~13× realistic ceiling on 12P+4E) — documented, not fixable by config.

## 5. Publish / community-extension mechanics (`COMMUNITY_EXT_PATH.md`)

1. Remove "waddle" template remnants from CI workflows + test filenames (S)
2. Real sqllogictest coverage: scalar fns, ParserExtension CREATE/DROP, error cases (S)
3. Platform scope decision — exclude wasm+windows vs port (M, Alok)
4. Repo layout — ext-cpp→root vs dedicated repo vs custom SOURCE_DIR (M, Alok)
5. `description.yml` (name, version, maintainers, ref, excluded_platforms) (S)
6. `make test` + code quality vs pinned ci_tools_version (S)
7. Windows port or graceful stub of serve_brain (M, optional)
8. PR to duckdb/community-extensions (S, last)

Risk noted: ParserExtension + internal C++ API churn per DuckDB release = medium-high
maintenance. Precedent: httpserver is in community, excludes only wasm.

## 6. Documentation & merge debts

- `edges_round5_draft.md` + `edges_round6_draft.md` — pending Alok merge into `edges.md`
- `docs/learning/00–08` (9 docs) — pending Alok review → then Notion mirror
- `readme_experiment/` bake-off — recommended merge (grok content into sonnet skeleton) awaiting
  Alok approval; README must gain "When NOT to use quackapi" (single-process blast radius,
  horizontal scaling until Quack multi-writer, OLAP-OOM from hostile queries, extension trust
  boundary) per report 05 §10
- `CHRONICLE.md` — regen is manual (`docs/build_pdf.sh`); no CI freshness check
- Stale-doc reconciliation: `TEST_PLAN.md:44` (shutdown "HARD" — actually shipped),
  ~~`FEATURE_GAP_MATRIX.md` (predates R2–R4 + B7)~~ **DELETED 2026-07-08 → replaced by the living
  `docs/STATUS.md`** (it was read as current and hallucinated shipped work as open). Frozen
  line-anchored citations to it still exist in `learning/08`, `WS_SPEC.md`, `POLISH_OPS_SPEC.md`,
  `ROADMAP_10M.md`, `TEST_PLAN.md` — those are historical study references to the pre-B7 snapshot,
  not live pointers; leave until their parent docs are reviewed. Also: access-log perf contradiction (§3.4)

## 7. Decision queue (Alok)

1. **Wave-A spec reviews** — TABLE_API (routes materialization tension), ADMIN_UI (asset
   embedding), SESSION_CSRF (CSRF model, admin bootstrap)
2. **Conformance coercion policy** — strict-vs-coerce rulings that close most of the ~17 BUGs
3. **Windows**: exclude vs port
4. **Repo layout** for community submission (root vs dedicated repo)
5. **Name**: quackapi vs duckapi (currently quackapi, locked informally 2026-07-03)
6. **Self-dispatch public reveal** in README (Claude recommends deliberate reveal; unanswered)
7. **TLS v2** (in-process mbedTLS) timing — v1 proxy path is unblocked regardless
8. **Multipart v2 mechanism** — base64 wrapper vs streaming parser
9. **edges round-5/6 merges, learning-docs review, README merge** (§6)
