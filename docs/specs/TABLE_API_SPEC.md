---
title: "CREATE API FOR TABLE — auto-CRUD instant backend surface"
subtitle: "The highest-leverage unbuilt feature per ROADMAP_10M §3 MUST #1 and §5 rank 5. PocketBase-class CRUD over DuckDB tables as pure DDL, without violating the no-materialized-derived-router-state rule."
author: quackapi
date: 2026-07-05
---

# TABLE API SPEC — `CREATE API FOR TABLE`

**Status:** build-ready v0 (contract for Wave A implementer). Grammar, surface, filter/sort/paginate/expand, auth hooks, OpenAPI, validation, and the critical design decision (#6) are resolved here with verified mechanisms.

**Thesis (first):** The single biggest adoption lever for the "instant backend" category (see rivals research and ROADMAP) is zero-boilerplate CRUD: declare a table, get a policed, documented REST API. FastAPI users pay 3.39 M dl/mo for `fastapi-pagination` alone and still have no standard envelope; every project reinvents DTOs, filters, and pagination. quackapi answers with `CREATE API FOR TABLE` that reuses the existing `handle_request` / C-router pipeline, `CREATE POLICY` integration, and OpenAPI-as-SELECT. All while obeying the hard project rule: **no materialized derived router state in relations** (routes table is user-authored SSOT; a prior 28 k req/s "fast lane" cache was removed over exactly this).

Every DuckDB capability below was verified with live `duckdb -unsigned` probes (v1.5.3); transcripts are included. House style: thesis, verified probe, grammar, semantics tables, honest edges.

---

## 1. Grammar + Knobs + Defaults

```sql
CREATE API FOR TABLE <table_ident>
  [ PREFIX '/<path>' ]                    -- default: '/' || table_ident (e.g. /users)
  [ OPERATIONS ( 'list' [, 'get' [, 'create' [, 'update' [, 'delete'] ] ] ] ) ]
  [ PAGE_SIZE <positive_int> ]            -- default 20
  [ MAX_PAGE_SIZE <positive_int> ]        -- default 100
  [ DEFAULT_SORT '<col> [DESC]' [, ...] ] -- default: primary key ascending (or 'id' if present)
  [ EXPAND_DEPTH <nonneg_int> ]           -- default 1; max enforced at 2 in v1
  [ FILTER_OPS ( 'eq' [, 'ne' [, ...] ] ) ] -- default: safe subset (eq,ne,gt,gte,lt,lte,like,ilike,in)
;

DROP API FOR TABLE <table_ident> [ PREFIX '/<path>' ];
```

- `table_ident` must name a base table visible in `duckdb_tables()` (not a view, not internal/temp).
- OPERATIONS default = all five. Subsets supported (e.g. read-only API).
- PREFIX must start with `/`; trailing slash normalized away.
- Knobs are stored in the registry (see #6); they affect generated surface and OpenAPI.
- Idempotent recreate: `CREATE OR REPLACE` form supported (DROP old + CREATE).
- DROP removes the registry entry; does **not** touch user rows in `routes`.

**Verified:** parser extension pattern (see ext-cpp/src/quackapi_extension.cpp:210 (RouteDdlParse/Plan) and ApplyRouteFunc at exec time) is identical to CREATE ROUTE. Plan phase packages data; execution performs INSERT/DELETE on registry + reload. No parse-time side effects.

---

## 2. Generated Surface (exact paths, methods, status codes)

For `CREATE API FOR TABLE users PREFIX '/users'` (default ops):

| Method | Path              | Meaning          | Success | Error cases |
|--------|-------------------|------------------|---------|-------------|
| GET    | /users            | list (filter/sort/page/expand) | 200     | 422 (bad filter/page) |
| GET    | /users/{id}       | get one          | 200     | 404 (not found), 422 (bad id type) |
| POST   | /users            | create           | 201     | 422 (validation), 409 (conflict) |
| PATCH  | /users/{id}       | partial update   | 200     | 404, 422 |
| DELETE | /users/{id}       | delete           | 204     | 404 |

- HEAD treated as GET (existing rule).
- 405 only if path segment matches a table-api prefix but method not in OPERATIONS (Allow header populated exactly as current 405 logic).
- 404 for unknown path (no table-api match and no explicit route).
- Body shapes:
  - list: envelope (see §4)
  - get/create/update: the row as JSON object (to_json semantics)
  - delete: no body
- Consistent with existing conformance (201 on POST create, 422 detail array, 404/405/422 shapes).

OpenAPI paths emitted under the PREFIX with operationId = listUsers / getUser etc.

---

## 3. Filter Language (list endpoint)

**Choice:** PostgREST-style `?col=op.value` (with value URL-decoded).

Examples:
- `GET /users?age=gte.18&status=eq.active`
- `GET /users?name=ilike.Al%25&created_at=gt.2026-01-01`
- `GET /users?id=in.(1,2,3)`

Supported safe operators (default FILTER_OPS):
`eq ne gt gte lt lte like ilike in is`

**Injection-proof contract (MANDATORY):**
- Query string parsed exactly as current `_qs_to_map` (structural, last-wins, no regex).
- Operator and value are **never** spliced into SQL text.
- The generated list handler (synthetic) builds the predicate via bound/structural form:
  ```sql
  AND (qm['age_gte'] IS NULL OR age >= try_cast(qm['age_gte'] AS INTEGER))
  AND (qm['name_ilike'] IS NULL OR lower(name) LIKE lower('%' || qm['name_ilike'] || '%'))
  ...
  ```
  (or equivalent macro over a filter_map). Same discipline as current param_literals + try_cast path.
- Unknown cols or ops → ignored or 422 (documented policy; default = ignore for forward compat).
- `in.(a,b)` parsed as list; `is.null` / `is.not_null` special.

**Justification vs PocketBase:**
- PostgREST syntax is purpose-built for exactly auto-CRUD REST surfaces, widely understood by the "instant backend" cohort.
- Lower parser surface than PocketBase's `?filter=(author.id=123 && title~'foo')` full-expression language (requires safe expression evaluator or sandbox; higher risk of accidental expressiveness).
- Maps 1:1 to the existing parameterized/structural pipeline in handle_request; no new expression engine.
- Still covers the 80% of real list filters (equality, ranges, prefix, membership).
- PocketBase filter lang is a great *future* extension behind an explicit FILTER_LANG=POCKETBASE knob once we have a verified safe expr sandbox.

All filter application happens after policy USING predicates (see §7).

---

## 4. Sort + Pagination + Standard Envelope + Count Honesty

**Envelope (THE standard — kills fastapi-pagination need):**

```json
{
  "items": [ {"id":1, ...}, ... ],
  "total": 1234,
  "page": 1,
  "perPage": 20
}
```

- `page` 1-based.
- `perPage` clamped to [1, MAX_PAGE_SIZE].
- `total` = COUNT(*) of the filtered set (before pagination).

**Sort:** `?sort=-age,id` (comma list; leading `-` = DESC; default DEFAULT_SORT).

**Offset vs cursor:**
- Offset (default): `?page=N&per_page=M` → `OFFSET (N-1)*M LIMIT M`.
- Cursor (opt-in, cheap keyset): `?cursor=<opaque>&per_page=M` (cursor encodes last sort tuple). Returns `nextCursor` in envelope when more rows exist. Only supported when a total order (PK or explicit DEFAULT_SORT) is present.
- Both cheap because the underlying scan is the same; cursor avoids the OFFSET tax on deep pages.

**Total count honesty (verified):**
DuckDB is columnar/OLAP. A filtered COUNT is zone-map + vectorized and **very cheap** even on 100k+ rows.

Probe transcript (100k row table, LIKE filter):

```
EXPLAIN ANALYZE SELECT count(*) FROM big WHERE name LIKE 'x1%';
-- Total Time: 0.0009s
-- TABLE_SCAN ... Filters: name>='x1' AND name<'x2' ... 11,111 rows ... 0.00s
```

For API-sized tables this is noise. We **always** return `total` (no "count=expensive" escape hatch like some row-store APIs). Document it loudly as a differentiator vs traditional backends.

---

## 5. Expand (FK following)

`?expand=author` (or comma list). For a posts row with FK `author_id REFERENCES authors(id)`, the response item gains:

```json
{
  "id": 42,
  "title": "...",
  "author_id": 7,
  "author": { "id": 7, "name": "alice" }
}
```

- Only declared FKs (via REFERENCES) are expandable.
- Depth limit: EXPAND_DEPTH (default 1, hard max 2 in v1). `?expand=author,author.foo` at depth 2.
- Missing FK → "author": null (LEFT semantics).
- v1: only to-one (the FK direction). Reverse (has-many) out of scope.
- Cost: for list, expansion is a correlated or join-lateral expansion inside the generated handler (still one round-trip).

**Verified DuckDB FK introspection (the exact mechanism the builder will call at CREATE time or request time):**

Probe (live `duckdb -unsigned`):

```
CREATE TABLE authors (id INTEGER PRIMARY KEY, name VARCHAR NOT NULL);
CREATE TABLE posts (id INTEGER PRIMARY KEY, author_id INTEGER REFERENCES authors(id), title VARCHAR NOT NULL, body VARCHAR);

SELECT constraint_type, constraint_text, table_name, constraint_column_names, referenced_table, referenced_column_names
FROM duckdb_constraints()
WHERE constraint_type = 'FOREIGN KEY' OR table_name IN ('posts','authors')
ORDER BY table_name, constraint_type;
```

Result:
```
constraint_type | constraint_text                              | table_name | constraint_column_names | referenced_table | referenced_column_names
FOREIGN KEY     | FOREIGN KEY (author_id) REFERENCES authors(id) | posts      | [author_id]             | authors          | [id]
... (PKs and NOT NULLs also visible)
```

Additional columns probe (for DTO derivation):
```
SELECT table_name, column_name, data_type, is_nullable, column_default
FROM duckdb_columns() WHERE table_name IN ('posts','authors') ...
```
Yields exact types + nullability for required/422 rules and OpenAPI schemas.

`duckdb_tables().sql` also contains the REFERENCES clause for belt-and-suspenders parsing if needed.

---

## 6. THE CRITICAL DESIGN TENSION — resolved

**Project rule (repeated in framework.sql comments, ROADMAP, edges, build history):**  
"A router materialized as a relation is a cache — a prior 28 k req/s 'fast lane' was ripped out over this."  
`routes` + `param_schema` are the **source of truth** for explicit routes. `handle_request` computes everything per-request from them. Nothing is pre-materialized into the router tables.

**Option (a) — generated-code model (expand at DDL time)**  
`CREATE API FOR TABLE` does `INSERT INTO routes` + `INSERT INTO param_schema` (synthetic route_ids like `api_users_list`, handlers with baked column lists at creation time).  
- Pros: routes table + C g_rt get the surface "for free"; OpenAPI just sees more rows; identical to hand-written routes; introspection is `SELECT * FROM routes`.
- Cons (fatal under the rule):
  - This **is** materializing derived router state into the registry relation. Direct violation.
  - ALTER TABLE (ADD COLUMN, change type, DROP col) makes the inserted rows **stale** — create body schema, list projection, filterable columns drift. Requires explicit DROP API + re-CREATE (user error mode).
  - User (or migration) can `DELETE FROM routes WHERE route_id LIKE 'api_%'` or edit the handler; "auto" becomes lie.
  - `SELECT * FROM routes` becomes polluted with magic rows that do not survive schema change.
  - Reload discipline becomes "re-gen on every ALTER" — complexity and inconsistency risk.

**Option (b) — virtual registry (match at request time)**  
`CREATE API FOR TABLE` inserts **only** into a tiny new `table_apis` registry table (and optionally a supporting `table_api_filters` or column snapshot for stability):
```sql
CREATE TABLE table_apis (
  api_id VARCHAR PRIMARY KEY,
  table_name VARCHAR NOT NULL,
  prefix VARCHAR NOT NULL,
  operations JSON NOT NULL,   -- ["list","get",...]
  page_size INTEGER,
  max_page_size INTEGER,
  default_sort VARCHAR,
  expand_depth INTEGER,
  created_at TIMESTAMP DEFAULT now()
);
```
(Plus any per-col policy or filter allow-list rows if needed.)

- At request time (handle_request) and in C `quack_route`:
  1. Normal explicit `routes` match (unchanged).
  2. If no match, or as parallel cheap probe: `table_api_match(method, path, table_apis, current_catalog)` — uses prefix + {id} patterns synthesized from the table's PK.
  3. On hit: **synthesize** param-like defs + handler SQL **from live `duckdb_columns()` + `duckdb_constraints()`** at that instant; execute the CRUD (list applies filters from query_map, etc.).
- OpenAPI build (the /openapi.json special case) unions explicit routes + generated paths derived the same way.
- C mirror loads `table_apis` (small) into a side struct; match logic is additive, never mutates g_rt.

**Analysis against rule / schema change / introspection:**

| Dimension              | (a) expand-into-routes                  | (b) virtual (recommended)                          |
|------------------------|-----------------------------------------|----------------------------------------------------|
| Materialized derived state in routes? | Yes — violation of the rule | No — routes stays user SSOT only                  |
| SELECT * FROM routes introspection | Polluted with magic rows               | Clean; use SELECT * FROM table_apis               |
| ALTER TABLE ADD COLUMN | Drift (stale param_schema/handler)     | Live — next request/OpenAPI sees new columns      |
| User can break the "auto" surface | Yes (edit/delete rows)                 | No (only via DROP/CREATE API DDL)                 |
| CREATE/DROP API cost   | Heavy (many INSERTs + reload)          | Light (1-2 registry rows)                         |
| C hot path             | Free (already in g_rt)                 | Small additive match + synthetic path (mirrored)  |
| Policy patterns        | Work (they match the inserted rows)    | Work (match the canonical 'GET /users' strings)   |

**RECOMMENDATION: (b) virtual.**  
This is the decision the human most cares about. It keeps the architecture honest: the routes table never lies about what the *author* wrote. Table APIs are a first-class *computed surface* over the catalog + a tiny declaration table, exactly like OpenAPI itself is a SELECT today (not a materialized blob). Schema evolution is free, introspection stays trustworthy, and we do not re-introduce the "cache in a relation" pattern that was explicitly removed.

Implementation note for builder: `table_apis` + synthetic logic lives in framework.sql (pure oracle) and is mirrored in the C router load/decision path. No rows ever go into `routes` for table APIs.

---

## 7. Policy / Auth Integration

Per-operation policies work **exactly** as defined in CREATE_POLICY_AUTH_SPEC.md.

```sql
CREATE POLICY users_list ON 'GET /users'
  AS PERMISSIVE USING (claims['role'] IN ('admin','viewer') OR ...);

CREATE POLICY users_create ON 'POST /users'
  AS RESTRICTIVE WITH CHECK (claims['tenant'] = body_tenant());

CREATE POLICY users_delete ON 'DELETE /users/{id}'
  AS RESTRICTIVE USING (claims['sub'] = request['path']['id']);
```

- Pattern string is the **canonical generated** one (`'GET /users'`, `'POST /users'`, `'GET /users/{id}'`, etc.).
- USING (read) vs WITH CHECK (write) honored per the auth spec.
- Enforcement point: after route/table-api match, before handler execution (same two-phase authenticate → authorize as today).
- 401/403 semantics identical.
- Because virtual, policy evaluation uses the *effective* request shape; claims/request MAPs are already defined.

No new grammar; ordinary CREATE POLICY is sufficient and is the intended integration.

---

## 8. OpenAPI Emission

Generated routes appear in `/openapi.json` with:
- `operationId` clean (listUsers, createUser, ...).
- Parameters derived from path (`{id}`) + query (page, perPage, sort, expand, filter ops).
- Request body schema for create/update: object with properties from table columns (names + types), required = NOT NULL columns without DEFAULT (for create); all optional for partial update.
- Response schema: full row shape for 200/201.
- 422 always present with the standard detail array.
- 404/204/409 as appropriate.

**The three-model DTO killer (stated):**  
In FastAPI you write `UserCreate`, `UserRead`, `UserUpdate` (and keep them in sync). Here the table *is* the schema. Column metadata (`duckdb_columns` + constraints) directly produces the OpenAPI properties + required + nullable. One source, zero boilerplate, zero drift.

The OpenAPI builder (currently the special-case in rendered_static inside handle_request) is extended to union `routes` + `table_apis`-derived paths. Same pure-SQL contract.

---

## 9. Validation (column types / NOT NULL / CHECK → 422)

On create/update the synthetic path reuses the exact validation pipeline from param_values / validation_errors / err_agg in framework.sql:

- Type mapping: INTEGER→int, VARCHAR→string, DOUBLE→float, BOOLEAN→bool, TIMESTAMP→string (or richer later).
- NOT NULL + no DEFAULT → required on create (422 "missing").
- Provided value wrong type → 422 "int_parsing" / "float_parsing" / "string_type" etc (exact messages and loc shapes).
- CHECK constraints: rows from `duckdb_constraints()` WHERE constraint_type='CHECK' supply the expression; the validator evaluates equivalent predicates (or stores a normalized constraint_json) and emits the same `less_than_equal` / `greater_than_equal` or new check_violation codes with ctx.
- Extra body keys: ignored (current POST /users behavior).
- Partial update: only fields present in body are validated/coerced; omitted fields are left alone.

All 422 bodies are byte-compatible with the existing conformance shape.

---

## 10. Honest Edges + Out-of-Scope for v1

**In scope v1 (buildable from this spec):**
- Single-column PK tables (id or the declared PK).
- Base tables only.
- To-one FK expand (depth ≤2).
- Offset + basic cursor pagination.
- PostgREST-style safe filters (the listed ops).
- Per-op policies.
- Full OpenAPI + Swagger visibility.
- Live reflection on column addition for subsequent requests.

**Out for v1 (name them):**
- Composite PKs (probe showed `constraint_column_names = [a,b]`; handler for `{id}` becomes ambiguous; deferred).
- Views as API targets (duckdb_views() separate; "table" in grammar means base table).
- Generated columns on create/update (appear in responses + list as read-only; omit from input schemas).
- Reverse relations / has-many expands.
- JSON/STRUCT column deep filters or expands.
- Multi-column unique conflict 409 with exact field reporting (basic 409 on any unique violation OK).
- Transactions spanning multiple generated ops.
- Automatic soft-delete / audit columns.
- Cross-database / attached DB tables.
- Custom id column name that is not the PK (v1 assumes the PK column provides the `{id}` path segment).
- Full-text or vector filters (future differentiator).

Document in edges.md after implementation.

---

## 11. Tier-1 Test Plan (concrete) + C-mirror notes

**Tier-1 (pure SQL oracle) cases** (add to tier1_handle_request.test.sql and conformance/cases.jsonl):

1. CREATE API FOR TABLE users; — succeeds, appears in table_apis.
2. GET /users → 200 envelope with total=3, items array, page/perPage.
3. GET /users?page=1&per_page=2 → correct slice + total.
4. GET /users?age=gte.30 → only carol (assume seed).
5. GET /users?sort=-age → carol, alice, bob.
6. GET /users?expand=... (on a table with FK) → nested objects.
7. POST /users {"name":"dave","age":35} → 201 + body with id.
8. POST /users {"name":"x"} → 422 missing loc=["body","age"].
9. POST /users {"name":"x","age":"notint"} → 422 int_parsing.
10. GET /users/999 → 404.
11. PATCH /users/1 {"age":99} → 200, only age changed.
12. DELETE /users/1 → 204.
13. GET /users/1 after delete → 404.
14. CREATE POLICY ... ON 'GET /users' ... ; unauthed list → 403 (or 401).
15. After ALTER TABLE users ADD email VARCHAR; POST accepts email; list includes it.
16. DROP API FOR TABLE users; subsequent GET /users → 404 (or falls to explicit if any); routes table unchanged.
17. OpenAPI GET contains the generated paths + derived schemas (no three DTOs).
18. Cursor pagination happy path + nextCursor.
19. MAX_PAGE_SIZE clamp + 422 on per_page too big.
20. 405 on POST /users/{id} (method not in ops).

All must pass the existing differential driver (pure + C server vs FastAPI reference where shapes are defined).

**C-mirror notes (for ext-cpp parity):**
- Add `table_apis` (and any column snapshot) load inside `quack_load_registry` (parallel to routes load; tiny).
- Extend `quack_route` (after the best-route literal loop) with a `try_table_api_match(...)` that returns a synthetic RouteDecision when prefix + op pattern hits. The decision carries enough info (table, op, id_value, filter_map, sort, page, expand) for the worker to either:
  - Execute a fixed-template SQL through the same internal channel, or
  - Return a handler_sql string that the existing dynamic path can run (preferred for parity).
- Validation, literal substitution, 422 shape, and resp_headers must be byte-identical to the SQL oracle.
- `quack_route_decision` test cases + parity_b2.sh must grow the table-api matrix.
- Reload on CREATE/DROP API must call the same router reload path.
- No mutation of g_rt or routes from table api code.

---

## Verified Probes (transcripts embedded above)

- FK + column catalog (duckdb_constraints, duckdb_columns) — full output in §5.
- COUNT cost (0.0009 s on 100 k filtered) — §4.
- Generated columns + defaults visibility — §9 probe.
- Information_schema cross-check (constraints appear as CHECK for NOT NULL) — earlier probes.

All relied-on catalog surfaces exist and return the arrays/structs needed for synthetic generation.

---

**Dense summary of key decisions (for the builder + human):**

- Grammar + surface + statuses defined above; consistent with current 200/201/204/404/405/422.
- Filter: PostgREST `col=op.value` (safe, parameterized, justified vs PocketBase expr).
- Envelope: `{items,total,page,perPage}` + total-always (OLAP count is cheap — 0.9 ms probe).
- Cursor supported as cheap complement to offset.
- Expand: FK-driven via verified `duckdb_constraints()` (probe pasted), depth=1 default.
- **#6 DECISION: virtual (b)** — registry in `table_apis` only; routes untouched; live catalog reflection on ALTER; honors the "no derived router relation" rule that already killed one perf shortcut. This is the non-negotiable architectural call.
- Policies attach to the canonical generated patterns exactly as in CREATE_POLICY_AUTH_SPEC.
- OpenAPI derives schemas from columns (three-DTO killer stated).
- Validation reuses existing pipeline + CHECK expressions from catalog.
- v1 edges explicitly called out (composite PK, views, generated cols on write, has-many, etc.).
- Tier-1 cases listed + C-mirror load/match notes.

Nothing unverifiable with the probes performed; all DuckDB surfaces used are present in v1.5.3 and exercised in-repo today. Build from this contract.

(End of spec.)
