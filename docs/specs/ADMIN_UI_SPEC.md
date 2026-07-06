---
title: "ADMIN_UI_SPEC — /_admin dashboard served from the extension"
subtitle: "The 60-second demo adoption lever (PocketBase parity). Introspection-first, zero derived state, auth-gated."
author: quackapi
date: 2026-07-05
---

# ADMIN_UI_SPEC — /_admin dashboard

**Status:** build-ready spec (Wave A centerpiece per ROADMAP_10M §3 MUST #4 and §5 rank 7; FastAPI bolt-on #7 "loudest Django miss").

**References (read first per task):** docs/ROADMAP_10M.md (§3 #4, §5 ranks 6/7), docs/research/instant-backend-rivals-2026-07-05.md (PocketBase admin + adoption drivers), docs/research/fastapi-pain-2026-07-02/07-bolton-ecosystem.md §3 (admin), docs/specs/CREATE_POLICY_AUTH_SPEC.md (auth integration + house style), framework.sql (routes + tera HTML + rendered_static), ext-cpp/src/quackapi_brain.cpp (static_body / kind=static|html zero-DB path + route_headers Set-Cookie), docs/specs/TABLE_API_SPEC.md (parallel dep for data grid CRUD surface).

**Thesis**

PocketBase's baked-in admin UI is repeatedly cited as the single biggest "wow" and 60-second demo driver (rivals research: "Admin UI = 60-second demo-ability — the single biggest wow factor"; HN/ShowHN threads rank it #3 after "one file, zero deps"). FastAPI users quantify "30 min in Django vs 2–3 days in FastAPI" and defect over the gap. quackapi's answer must be **the artifact itself serves a functional /_admin** — no second binary, no external process, zero deps.

Everything the admin shows is already queryable state: `duckdb_tables()` / `duckdb_columns()`, the `routes` + `param_schema` + `route_headers` registry, future `quackapi_*` auth/policy tables (CREATE_POLICY_AUTH_SPEC), and access/query surfaces that are (or will be) tables. The admin is a **viewer + thin mutator over the catalog**. No new derived state tables, no shadow caches. This is the "introspection-first" differentiator vs every other instant backend.

The delivery mechanism is the core architectural decision (see §1). Once chosen, the surface rides existing primitives: static_body / kind=html|static paths (framework.sql rendered_static + brain.cpp:2925), route_headers for cookies, tera for any server-rendered fragments, handle_request for all routing/422/auth, and (for data grid) the TABLE_API surface being specced in parallel.

## 1. Delivery mechanism — the core decision (researched + recommended)

**PocketBase mechanism (verified):** Go `//go:embed all:ui/dist` (or `app/build` for custom) of a prebuilt Svelte SPA. The admin UI is a real client-side app (Svelte + Vite build output) embedded at compile time into the single binary, served via http.FileServer over the embed.FS. Binary size impact accepted (~ tens of MB total for PocketBase); their release CI runs the frontend build then `go build`. See GitHub pocketbase/pocketbase/ui/embed.go and community examples using `//go:embed all:app/build`.

**Our constraints (quackapi-specific):**
- Community-extension CI builds the .duckdb_extension (C++ + DuckDB vcpkg tree, cross-platform docker images). Adding a Node.js step (pnpm/yarn build of a real SPA) is strongly dispreferred and risks the "single-artifact zero-config" story.
- Offline / zero-dep guarantee is table stakes (rivals: "ONE file, zero deps").
- We already have a working HTML route path (framework.sql:699 `kind IN ('openapi','html','static')` → `rendered_static`, brain.cpp static_body pre-render at router load + zero-DB serve at 2925, route_headers for Set-Cookie).
- tera ext is already LOADed in receipts (compose.sql:18, tera_render over live data).
- Binary size of the .duckdb_extension today is ~26 MB (release build with full symbols/pool).

**Options analyzed:**

(a) **Embed prebuilt static assets (HTML/JS/CSS byte arrays) into the .duckdb_extension binary at compile time.**
   - Analog to PocketBase: commit (or generate) a dist/ tree, cmake/xxd or C23 `#embed` turns files into `static const unsigned char admin_app[] = {0x...};`.
   - Pros: full rich SPA possible (React/Vue/Svelte output), interactive data grid with client state, offline, exact PocketBase parity in feel.
   - Cons: binary size delta = size of assets (a minimal functional admin SPA is 800kB–3MB min+gz; real PocketBase admin larger). Build pipeline: either (i) node step in CI (violates "no node in build path strongly preferred"), or (ii) prebuilt assets committed in repo (maintainer does `npm run build` locally once per wave, checks in the output blobs or a single bundle). Community CI remains C++-only if (ii). Still adds maintenance (asset updates, tree-shaking, sourcemaps?).
   - Offline/zero-dep: excellent.
   - Verdict: viable only with pre-commit assets; size + "is this a JS project now?" perception risk.

(b) **Server-render everything with the tera templating already in the stack.**
   - Routes of kind='html' or dynamic handlers returning tera_render(..., json_from_catalog) for every screen. Forms POST back and redirect (classic server-rendered CRUD).
   - Pros: zero new binary size beyond a few kB of templates (stored in a `admin_templates` table or literals), pure, no node ever, uses exactly the compose receipt path, leverages existing tera.
   - Cons: interactivity is page-reload or htmx (htmx would be another small embed or CDN — CDN breaks offline/zero-dep). A rich sortable/filterable/paginated data grid with inline edit modals is painful in pure server-render without JS. Does not deliver the "wow" of PocketBase's live grid in 60s. Future HTMX polish (Wave D) helps but is after the adoption lever.
   - Verdict: perfect for ultra-minimal ops pages or reports; insufficient for the category-defining admin demo.

(c) **Hybrid: one embedded single-file HTML app that talks to JSON routes.**
   - A single committed `admin.html` (self-contained: inline Tailwind via play CDN or (preferred) minimal hand-crafted CSS + vanilla JS ~100–400 kB total for v1; no external script tags for zero-dep). Served as a static_body route for `GET /_admin*` (SPA fallback to index for client routes). The JS does fetch() to JSON endpoints:
     - catalog queries surfaced as routes or direct (but prefer routes for auth uniformity),
     - the TABLE_API CRUD surface (filter/sort/pag/expand) once landed,
     - routes/policies viewer as `SELECT * FROM ...`,
     - gated `/ _admin/sql` POST.
   - The single file is turned into a byte array at C++ build time via a node-free generator (cmake custom command using `xxd -i` which is ubiquitous in C++ toolchains, or a 20-line Python stdlib script, or even literal include of a generated .inc checked in). No node, no pnpm, no frontend CI step for the extension build.
   - Pros: rich interactive demo (client-side grid, modals, instant filter) matching PocketBase feel; tiny delta size (one file, compressible); offline/zero-dep (single response); reuses every existing static/HTML/static_body/route_headers path; build pipeline impact near-zero (asset is source-controlled like any .sql or .md).
   - Cons: the admin UI must be authored/maintained as a single file (or built once externally by maintainer and the output committed); JS must stay small for v1.
   - Offline/zero-dep: excellent (the HTML+JS is the response; no further loads for core).
   - Binary impact: +200–600 kB typical for a functional v1 (measured post-embed; far smaller than a full framework SPA).
   - Verdict: **RECOMMENDED**.

**Recommendation: (c) hybrid single-file embedded HTML app.**

Rationale: it is the only option that simultaneously (1) delivers the PocketBase-style 60s interactive demo that research proves drives adoption, (2) keeps the community-extension build path strictly C++ (no node), (3) adds minimal binary weight, (4) reuses the exact static/HTML serving machinery already shipping and parity-tested, and (5) stays introspection-pure (the JS only calls SELECTs and the TABLE_API routes; no new server state). (a) is acceptable later if we decide a richer SPA is worth the asset burden; (b) is a fallback for zero-JS environments.

**Implementation sketch (no source change here):**
- Add `assets/admin.html` (or generated at configure time) as the single file.
- At router load (quack_load_router_using_connection / load_registry), or via a dedicated static registration, INSERT a route with kind='static' or 'html', pattern='/_admin', handler= the literal (or pre-read), and pre-render into static_body exactly as /health and /docs do today (brain.cpp:3190 and framework.sql:699).
- Subpaths or client-side routing: `/_admin/*` can fall back to the same file (SPA) or register additional lightweight routes.
- Any supporting CSS/JS is inlined in the single file. (If we ever need an immutable asset, use route_headers + Cache-Control and a content-hash route.)
- Future: the same mechanism serves the "SQL console" iframe or panel.

## 2. Feature set v1 (ruthlessly scoped to the 60-second demo)

**v1 exit criteria:** `LOAD quackapi; SELECT quackapi_serve(...);` → open http://127.0.0.1:PORT/_admin → see schema, browse a table's rows, create/edit/delete a row, view routes/policies, run a gated SQL statement, see live-ish stats. All without leaving the browser. Auth bootstrap works on first run.

**In scope for v1 (MUST):**
- **Schema browser:** list of tables/views (from duckdb_tables(), duckdb_views()), columns (duckdb_columns() or pragma_table_info), row counts (count(*) or estimated via table_stats where present; for v1 accept a small scan cost or cache-in-render for demo tables). FK relationships for later expand.
- **Data grid with CRUD + filter/sort/pagination:** for any table. This **rides the CREATE API FOR TABLE surface** (TABLE_API_SPEC.md parallel dep). The admin JS calls the generated REST endpoints (GET /tables/{name}?filter=...&sort=...&page=...&per_page=..., POST/PATCH/DELETE). Do not duplicate the CRUD grammar here; the admin is a consumer + viewer. Filter/sort/pag envelope must be the standard one defined in the API spec.
- **Routes / policies / auth-schemes viewer:** read-only nice tables over `routes`, `param_schema`, `route_headers`, and the quackapi_auth / policies tables from CREATE_POLICY_AUTH_SPEC. "Who can call what" at a glance.
- **SQL console (gated):** a textarea + Run. Executes against the same connection semantics as handlers (subject to the active auth claims + policy). Only reachable after successful admin auth. Result rendered as table (first N rows) + "download CSV/JSON" (simple). Destructive statements allowed only for admins (policy). 422/ errors shown inline.
- **Server stats:** req/s (simple moving average or from internal counters exposed as a tiny table), access log tail (the C access-log ring or its surfaced table form; at minimum a last-50 recent lines view or a `SELECT * FROM access_log ORDER BY ts DESC LIMIT 50`). "These are already tables" per roadmap — admin just renders them. Health indicators (pool saturation, inflight, etc.) via existing diagnostics.

**Explicitly out for v1 (parking lot / v2):**
- User/role management UI (beyond first admin bootstrap).
- Bulk import/export wizards.
- Realtime subscription inspector.
- File storage browser.
- Policy/rule editor (create/edit; v1 is view + the DDL surface).
- Multi-db / attach browser.
- Theming, saved queries, full query history, user preferences.
- Anything requiring new server-side derived state or background jobs.
- Mobile/responsive polish beyond "usable".
- OAuth provider config UI.

**Introspection-first thesis (no new derived state)**

Every screen is literally one or more SELECTs over catalog/registry tables the server already maintains. Examples (exact queries will be refined in impl; the contract is "no shadow tables"):

- Schema browser (tables list): `SELECT database_name, schema_name, table_name, column_count, estimated_size, has_primary_key FROM duckdb_tables() ORDER BY table_name;`
- Columns for a table: `SELECT * FROM duckdb_columns() WHERE table_name = ?;`
- Row count (per-table, on demand or small-N): `SELECT (SELECT COUNT(*) FROM "<table>") AS row_count;` (or use stats extension when present; admin tolerates cost for demo tables).
- Routes + params: `SELECT r.route_id, r.method, r.pattern, r.kind, r.status, list(p.name) AS params FROM routes r LEFT JOIN param_schema p USING (route_id) GROUP BY ...;`
- Policies: `SELECT * FROM quackapi_policies;` (per CREATE_POLICY_AUTH_SPEC).
- Auth schemes: `SELECT * FROM quackapi_auth;`
- Recent access (log tail): `SELECT * FROM access_log ORDER BY logged_at DESC LIMIT 100;` (or the equivalent surface once the ring is queryable; v1 can surface the C ring via a tiny table func if needed — still "existing").
- Server stats / reqs: counters exposed via a `quackapi_stats()` view or direct internal SELECTs (e.g., inflight from g_inflight, pool gens, etc.). Roadmap states they are tables.

The admin never INSERTs into its own state tables; mutations go through the public DDL (CREATE/DROP ROUTE, CREATE POLICY, the TABLE_API DML) or the gated SQL console. This guarantees the admin is always a true reflection and that the same queries work from any client (psql, the console itself, external tools).

## 3. Security

**/_admin MUST be auth-gated.**

- All paths under `/_admin` are protected by a `CREATE POLICY` (or a built-in "admin" scheme) that requires verified claims with an admin role / flag. See CREATE_POLICY_AUTH_SPEC for predicate form: `USING (claims['role'] = 'admin' OR claims['is_admin'] = true)`.
- The routes themselves are registered normally (so they appear in the routes viewer and OpenAPI if desired).
- Cookie auth (from sibling SESSION_CSRF_SPEC) + CSRF protection apply to the HTML UI. Pure Bearer token routes used by the admin's fetch() calls are exempt from CSRF (see SESSION spec).
- Bind address interaction: default remains 127.0.0.1 (already the case). Exposing /_admin on 0.0.0.0 requires explicit opt-in and is documented as "only with TLS + strong auth".

**Bootstrap story (first-run admin creation — the PocketBase analog)**

On first boot (no rows in the admin users table, or a dedicated `quackapi_admins` / users-with-role table has zero admins):
- The server prints exactly once to stdout/stderr (before or at the "listening" line):
  ```
  quackapi: no admin users found. Create the first via:
    http://127.0.0.1:PORT/_admin/bootstrap?token=4f3c...9a2b
  (single-use, expires in 10 minutes; or set QUACKAPI_BOOTSTRAP_TOKEN=... and POST /_admin/bootstrap)
  ```
- The `/ _admin/bootstrap` endpoint (or the setup flow) accepts the printed token (or the env var), allows exactly one admin creation (INSERT into the user/admin table + any initial policy wiring), then invalidates the token. Subsequent hits 410/403.
- After creation: normal auth flow (the created admin can log in via the session scheme or bearer).
- Alternative headless: `QUACKAPI_ADMIN_USER=... QUACKAPI_ADMIN_PASS=...` (or equivalent DDL at boot) can pre-create; the print is suppressed.
- The bootstrap route itself is only active in the "no admins" state; after, it is gone or 404s.
- All of this is implemented with ordinary routes + a tiny state table (or a marker in the users table) + the same claims machinery. No special C++ backdoor.

**CSRF dependency:** full details in SESSION_CSRF_SPEC. The admin HTML UI (forms + fetch POST/PUT) will send the CSRF token (synchronizer pattern). The spec must ensure cookie-authenticated admin sessions are protected while pure-Bearer API clients are not burdened.

## 4. Honest edges, binary-size budget, build plan sketch + verification gates

**Honest edges (say loudly):**
- v1 admin is scoped; it is a *demo* and ops accelerator, not a replacement for a custom frontend.
- Row counts on large tables in the schema browser will be slow or approximated; the data grid uses the proper paginated API.
- SQL console is powerful and gated only by auth/policy — a compromised admin account = full DB access (same as any admin UI; document).
- The single-file HTML will be minimal vanilla/htmx-class for v1. Full Svelte parity is v2+ if ever.
- No automatic "export this view as route" from the admin in v1 (that is a nice future).
- Binary size: every embedded byte ships to every user of the .duckdb_extension. Keep the admin asset under ~600 kB uncompressed for v1.
- Until TLS_SPEC lands (Wave B), cookie-based admin login on non-localhost is plaintext risk (document; recommend 127.0.0.1 or Wave B).

**Binary-size budget:**
- Current release .duckdb_extension ≈ 26 MB.
- v1 admin asset target: ≤ 600 kB added to the final binary after embed (realistic for a functional single-file grid + forms + one or two small panels). Measure with `ls -l` of the built extension before/after and a `strings | wc` or size diff in CI gate.
- Compression note: the extension binary itself is not compressed on disk for load speed; the HTML can be served with future gzip (POLISH_OPS) but the bytes are in the .so.

**Build plan sketch (high level, for implementer; no source edits in this task):**
1. Add the single admin asset (admin.html) under a new assets/ or ui/ dir. It is plain text checked in.
2. Extend the router registration (or a new static registration path) to insert the /_admin routes with kind='html'/'static' and body = content of the file. Ensure pre-render path (brain.cpp load path + framework rendered_static) captures it so zero-DB serves for the HTML shell.
3. Wire the asset bytes via cmake (xxd -i or equivalent generator that does not invoke node; the generator can be a one-off maintainer step or pure-stdlib script).
4. Add minimal supporting routes (or rely on TABLE_API + catalog SELECT routes) for the grid, console, etc. Register them with the appropriate policies.
5. Bootstrap flow: small table + token issuance + one POST handler.
6. Verification gates (must pass before claiming done):
   - `make test` (or the equivalent) still green; no regression in existing static/html/openapi paths.
   - Fresh build of the extension succeeds with the asset embedded; size delta recorded in a build log artifact.
   - Manual + scripted: `LOAD ...; SELECT quackapi_serve(...)` background; `curl -s http://127.0.0.1:PORT/_admin | head -c 200` returns 200 + HTML containing expected markers ("schema", "routes", etc.).
   - Auth bootstrap: start with empty admins → observe the stdout token line → curl the bootstrap URL with token → verify admin row created and subsequent /_admin requires auth.
   - Data grid smoke: once TABLE_API present, create a table via DDL or console, see it in schema, insert 3 rows via grid, see them, edit one, delete one, filter works.
   - SQL console: gated (401/403 before login), executes a SELECT and a safe INSERT, errors shown.
   - Stats: /_admin shows at least a req count or recent log line that matches a just-made request.
   - Conformance / parity: no new 422 or routing surprises for the admin paths.
   - Offline: after build, the extension file + a fresh duckdb with only the ext can serve /_admin with no network.

**v2 parking lot (for later specs):** full policy editor, user management, realtime inspector, bulk ops, theming, saved queries, multi-tenant admin views, embedding hooks for custom admin panels, dark mode, etc.

This spec is intentionally narrow so the Wave A demo can ship. The power comes from the fact that once TABLE_API + AUTH + SESSION + POLICY land, the admin is mostly "SELECTs + a nice HTML/JS shell".

---

**End of ADMIN_UI_SPEC**
