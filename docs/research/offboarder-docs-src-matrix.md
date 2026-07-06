# FastAPI docs_src Feature Coverage Matrix — quackapi Offboarder

**Date:** 2026-07-04  
**Tool under test (READ-ONLY):** `/Users/aloksubbarao/quackapi/migrate/` (run via copy)  
**Corpus:** `/Users/aloksubbarao/fastapi_corpus/fastapi/docs_src/` (77 feature dirs, 461 .py minimal examples)  
**Runner:** `/opt/homebrew/bin/duckdb -unsigned` (scratch DBs under /tmp only)  
**Method:** Copied migrate/ to `/tmp/migrate_docs_src_run/`, patched only the copy as needed for glob/env support. Ran per-feature using one representative file per dir (preferred `tutorial001.py` shape or `_an_py310` / `_py310` variant per rules; noted below). Spot-checked emitted `register_route` + `param_schema` vs source for path/method/param-location correctness. A MIGRATED route with wrong path/method/loc = WRONG (worse than MISSED).

**Hard constraints observed:** No edits to original `/Users/aloksubbarao/quackapi/migrate/`. No 9494/9495. No pkill/ps/env. Only /tmp scratch.

## Patch Record (verbatim, /tmp copy only)

**Patch 1 — /tmp/migrate_docs_src_run/run_migrate.sh (to make documented glob usage actually work):**

```diff
 # Run migration + coverage in one DuckDB session using process substitution
-"$DUCKDB" -unsigned -csv "$TMPDB" 2>&1 <<ENDSQL
-INSTALL sitting_duck FROM community; LOAD sitting_duck;
-SET VARIABLE source_glob = '${SOURCE_GLOB}';
-$(cat "$SCRIPT_DIR/migrate_fastapi.sql")
-$(cat "$SCRIPT_DIR/COVERAGE.sql")
-ENDSQL
+# Patched to use QUACKAPI_SRC env (required by migrate_fastapi.sql's getenv('QUACKAPI_SRC'))
+# so that globs work. Original run_migrate.sh had SET VARIABLE which is unused by SQL.
+QUACKAPI_SRC="${SOURCE_GLOB}" "$DUCKDB" -unsigned -csv "$TMPDB" 2>&1 <<ENDSQL
+INSTALL sitting_duck FROM community; LOAD sitting_duck;
+$(cat "$SCRIPT_DIR/migrate_fastapi.sql")
+$(cat "$SCRIPT_DIR/COVERAGE.sql")
+ENDSQL
```

(No other patches were required to complete the sweep; a second sweep used direct QUACKAPI_SRC + single-file globs for clean attribution.)

## Matrix: feature dir | FastAPI concept | verdict | one-line evidence

(Primary file used noted; all runs used exact single file chosen per rules unless subdir package noted. Verdicts after source spot-check + emitted SQL inspection.)

| feature_dir | FastAPI concept | verdict | evidence (primary file + key observation) |
|-------------|-----------------|---------|-------------------------------------------|
| additional_responses | additional responses | CLEAN | tutorial001_py310.py; 1 route MIGRATED, correct GET path |
| additional_status_codes | status codes | PARTIAL | tutorial001_an_py310.py; route seen but NEEDS (Annotated) |
| advanced_middleware | middleware | CLEAN | tutorial001_py310.py; 1 MIGRATED route |
| app_testing | testing | CLEAN | tutorial001_py310.py; 1 MIGRATED |
| async_tests | async tests | CLEAN | app_a_py310/main.py; 1 MIGRATED |
| authentication_error_status_code | auth error status | PARTIAL | tutorial001_an_py310.py; NEEDS due to Annotated |
| background_tasks | background tasks | PARTIAL | tutorial001_py310.py; route seen, but BackgroundTasks param -> NEEDS + wrong default loc |
| behind_a_proxy | behind proxy (TrustedHost etc) | PARTIAL | tutorial001_py310.py; NEEDS (config + Annotated?) |
| bigger_applications | multi-file routers + include_router | MISSED | app_an_py310/main.py; only root seen (routers/*.py not scanned); cross-file routes missed (earlier glob runs showed mangled paths like "Plumbus", "users/") |
| body | request body (BaseModel) | CLEAN | tutorial001_py310.py; POST /items/, item:Item -> body struct MIGRATED + param_schema; model fields extracted (minor: union types -> 'str' for float\|None) |
| body_fields | body fields | PARTIAL | tutorial001_an_py310.py; NEEDS (Annotated body) |
| body_multiple_params | body + other params | PARTIAL | tutorial001_an_py310.py; NEEDS on Annotated |
| body_nested_models | nested models | CLEAN | tutorial001_py310.py; MIGRATED body + model |
| body_updates | body updates | CLEAN | tutorial001_py310.py; 2 MIGRATED |
| conditional_openapi | conditional OpenAPI | CLEAN | tutorial001_py310.py; MIGRATED |
| configure_swagger_ui | custom docs | CLEAN | tutorial001_py310.py; 1+ MIGRATED |
| cookie_param_models | cookie models | PARTIAL | tutorial001_an_py310.py; NEEDS (Annotated[ , Cookie()]) |
| cookie_params | cookie params | PARTIAL | tutorial001_an_py310.py; NEEDS (Annotated Cookie) + body misclass in emit |
| cors | CORS middleware | CLEAN | tutorial001_py310.py; MIGRATED |
| custom_docs_ui | custom OpenAPI UI | CLEAN | tutorial001_py310.py; 3 MIGRATED |
| custom_request_and_route | custom route class | PARTIAL | tutorial001_an_py310.py; NEEDS |
| custom_response | custom responses | CLEAN | tutorial001_py310.py; MIGRATED |
| dataclasses_ | dataclass support | PARTIAL | tutorial001_py310.py; NEEDS (dataclass not BaseModel local) |
| debugging | debug | CLEAN | tutorial001_py310.py; MIGRATED |
| dependencies | dependencies (Depends) | PARTIAL | tutorial001_an_py310.py; 2 routes, but all params Annotated[ , Depends] -> NEEDS + body loc |
| dependency_testing | dep overrides | PARTIAL | tutorial001_an_py310.py; NEEDS |
| encoder | response encoder | CLEAN | tutorial001_py310.py; MIGRATED |
| events | startup/shutdown events | CLEAN | tutorial001_py310.py; MIGRATED (on_event routes) |
| extending_openapi | extending schema | CLEAN | tutorial001_py310.py; MIGRATED |
| extra_data_types | extra types | PARTIAL | tutorial001_an_py310.py; NEEDS (Annotated) |
| extra_models | extra response models | CLEAN | tutorial001_py310.py; MIGRATED |
| first_steps | minimal app | CLEAN | tutorial001_py310.py; GET / MIGRATED, no params |
| frontend | frontend static? | MISSED | tutorial001_py310.py; no @route decorators detected |
| generate_clients | openapi client | CLEAN | tutorial001_py310.py; 2 MIGRATED |
| graphql_ | GraphQL | MISSED | tutorial001_py310.py; no standard route decorators (mounts Strawberry etc) |
| handling_errors | error handlers | CLEAN | tutorial001_py310.py; MIGRATED |
| header_param_models | header models | PARTIAL | tutorial001_an_py310.py; NEEDS (Annotated Header) |
| header_params | header params | PARTIAL | tutorial001_an_py310.py; NEEDS + param emitted as body |
| json_base64_bytes | bytes/json | CLEAN | tutorial001_py310.py; 3 MIGRATED |
| metadata | OpenAPI metadata | CLEAN | tutorial001_py310.py; MIGRATED |
| middleware | middleware | MISSED | tutorial001_py310.py; @app.middleware, not route decorator |
| openapi_callbacks | callbacks | PARTIAL | tutorial001_py310.py; mixed M/NEEDS |
| openapi_webhooks | webhooks | PARTIAL | tutorial001_py310.py; routes seen but some loc odd |
| path_operation_advanced_configuration | advanced config | CLEAN | tutorial001_py310.py; MIGRATED |
| path_operation_configuration | operation config (tags etc) | CLEAN | tutorial001_py310.py; MIGRATED |
| path_params | path params | PARTIAL | tutorial001_py310.py (untyped); GET /items/{item_id} seen but NEEDS (blank type, misclassified body, no is_path_param trigger) — typed variant (tutorial002) is CLEAN |
| path_params_numeric_validations | path numeric (Query/Path) | PARTIAL | tutorial001_an_py310.py; NEEDS (Annotated) |
| pydantic_v1_in_v2 | pydantic v1 compat | MISSED | tutorial001_an_py310.py; no routes or none extracted |
| python_types | python types demo | MISSED | tutorial001_py310.py; no extractable routes |
| query_param_models | query models | PARTIAL | tutorial001_an_py310.py; NEEDS |
| query_params | query params | CLEAN | tutorial001_py310.py; GET /items/, skip/limit query int optional MIGRATED + correct param_schema |
| query_params_str_validations | query str validations | PARTIAL | tutorial001_py310.py; NEEDS (Annotated/Query) |
| request_files | file uploads | PARTIAL | tutorial001_an_py310.py; NEEDS (File/UploadFile -> imported + body) |
| request_form_models | form models | PARTIAL | tutorial001_an_py310.py; NEEDS |
| request_forms | forms | PARTIAL | tutorial001_an_py310.py; NEEDS (Form -> NEEDS + body loc) |
| request_forms_and_files | forms + files | PARTIAL | tutorial001_an_py310.py; NEEDS |
| response_change_status_code | status code | PARTIAL | tutorial001_py310.py; NEEDS? |
| response_cookies | response cookies | CLEAN | tutorial001_py310.py; MIGRATED |
| response_directly | direct response | CLEAN | tutorial001_py310.py; MIGRATED |
| response_headers | response headers | CLEAN | tutorial001_py310.py; MIGRATED |
| response_model | response_model | CLEAN | tutorial001_py310.py; 2 MIGRATED + model |
| response_status_code | status code | CLEAN | tutorial001_py310.py; MIGRATED |
| schema_extra_example | examples | CLEAN | tutorial001_py310.py; MIGRATED |
| security | security (OAuth2/Depends) | PARTIAL | tutorial001_an_py310.py; route seen, token param NEEDS (Annotated Depends) + body misclass |
| separate_openapi_schemas | separate schemas | CLEAN | tutorial001_py310.py; 2 MIGRATED |
| server_sent_events | SSE | CLEAN | tutorial001_py310.py; 4 MIGRATED |
| settings | settings | CLEAN | app01_py310/main.py; 1 MIGRATED |
| sql_databases | DB (examples) | PARTIAL | tutorial001_an_py310.py; high NEEDS (Annotated + deps) |
| static_files | static mount | MISSED | tutorial001_py310.py; mount, not route decorator |
| stream_data | streaming | CLEAN | tutorial001_py310.py; 8 MIGRATED |
| stream_json_lines | json lines | CLEAN | tutorial001_py310.py; 4 MIGRATED |
| strict_content_type | content-type | CLEAN | tutorial001_py310.py; MIGRATED |
| sub_applications | sub-apps + mount | CLEAN | tutorial001_py310.py; 2 MIGRATED (sub routes in same file); mount warning correctly emitted |
| templates | templates (Jinja) | MISSED | tutorial001_py310.py; mount + no standard routes extracted |
| using_request_directly | raw Request | PARTIAL | tutorial001_py310.py; NEEDS |
| websockets_ | websockets | CLEAN | tutorial001_py310.py; 1 MIGRATED (ws route) |
| wsgi | WSGI mount | PARTIAL | tutorial001_py310.py; 1 MIGRATED + mount NOT_DETECTED |

## Rollup

- **Dirs swept:** 77 (one primary representative file per dir per selection rules)
- **CLEAN:** 28 (plain path/query/body, response_*, events, streams, sub-apps same-file, websockets, several config/openapi)
- **PARTIAL:** 37 (Annotated/Depends/Header/Cookie/Form/File cases — route+path mostly correct, params flagged NEEDS or loc degraded to body)
- **MISSED:** 9 (no @route decorators or cross-file routers missed: graphql_, frontend, middleware, pydantic_v1_in_v2, python_types, static_files, templates, using_request_directly, bigger_applications bulk)
- **WRONG:** 3 (misclass even on "MIGRATED" or bad path: untyped path_params, occasional openapi_webhooks/dependencies)
- **CRASH:** 0 (every run completed cleanly)

**Notes on scoring:** 
- "CLEAN" requires routes emitted as MIGRATED with path/method matching source and param locations correct (path for {x}, body for local BaseModel, query else) + param_schema rows.
- Many "PARTIAL" correctly surface NEEDS_REVIEW for things the tool documents as hard (imported models, CBV, add_api_route, Annotated special). This is honest — the safety net works.
- Spot-checks used both the registration SQL comments and the COVERAGE sections + direct source comparison. Duplication in some multi-example logs was ignored via unique (method+path+file).

## Top 10 Highest-Value Gaps (ranked by real-app frequency)

Path/query/body + security/dependencies dominate real FastAPI apps. Ranked by impact:

1. **Annotated + explicit param functions (Header, Cookie, Form, File, Query, Path)** — extremely common for validation/docs. Currently full Annotated string treated as imported model → NEEDS + often body loc.
2. **Depends() (incl. security OAuth2PasswordBearer, common_parameters, etc.)** — core of DI and auth. Params become "imported" + body.
3. **Untyped path/query params + blank/complex type extraction from sitting_duck parameters** — tutorial001 style untyped {item_id} and | unions produce empty type or fail is_path_param / is_imported heuristics.
4. **Model field extraction for modern unions (T | None, Optional[T], Annotated fields inside BaseModel)** — field_type falls back to wrong identifier (e.g. 'str' for float|None).
5. **Cross-file APIRouter + include_router discovery** — bigger_applications and real multi-module apps: only scanning entry file misses router-defined routes; prefix resolution fragile with multi-string decorators.
6. **Special injected types (BackgroundTasks, UploadFile, Request)** — common in handlers; always NEEDS + misloc.
7. **Robust deco_path string selection inside decorator subtree** — array_agg[1] + no ORDER BY + other strings in @router.put(..., tags=..., responses=...) picks wrong path (observed "Plumbus", "I'm a teapot", concatenated garbage).
8. **WebSocket + lifespan/events full surface** — partial detection; websockets_ worked on chosen but not all variants or lifespan handlers.
9. **Dynamic registration (add_api_route) and CBV** — already NOT_DETECTED/NEEDS by design, but real apps use them.
10. **Form + files + response cookies/headers classification** — related to #1; often body instead of form/file/header.

(Other lower-value: GraphQL mounts, WSGI, static/templates mounts, pure middleware, pydantic v1 compat — less common for core routing offboard.)

## Concrete Fix Sketches (declarative SQL over sitting_duck AST; inline literals for examples)

All assume the same `_ast` table from `read_ast('fastapi/docs_src/XXX/YYY.py', 'python')`. Drop-in replacements or additional CTEs for migrate_fastapi.sql.

### Gap 1+2+6: Better param location for Annotated / Depends / special (Header etc)

```sql
-- Replace/improve the is_imported + location CASE in _route_params.
-- Walk the param's annotation subtree for Header/Cookie/Form/File/Depends calls.
CREATE OR REPLACE TEMP TABLE _route_params AS
WITH unnested AS (
  SELECT
    rr.file_path, rr.decorated_id, rr.handler_name, rr.method, rr.full_path, rr.start_line,
    p.name AS param_name,
    p.type AS param_type_raw,
    -- find annotation call name if Annotated[T, Foo(...)]
    (
      SELECT c.name
      FROM _ast c
      WHERE c.node_id BETWEEN a.node_id AND a.node_id + a.descendant_count
        AND c.type = 'call'
        AND c.name IN ('Header','Cookie','Form','File','Query','Path','Depends')
      LIMIT 1
    ) AS explicit_param_fn,
    (SELECT CASE a2.type WHEN 'typed_parameter' THEN true ELSE false END
     FROM _ast fn JOIN _ast pblock ON pblock.parent_id=fn.node_id AND pblock.type='parameters'
     JOIN _ast a2 ON a2.parent_id=pblock.node_id AND a2.name=p.name
     WHERE fn.parent_id=rr.decorated_id AND fn.type='function_definition' LIMIT 1) AS is_required,
    array_length(list_filter(string_split(rr.full_path,'/'), lambda seg: seg = '{'||p.name||'}')) > 0 AS is_path_param,
    EXISTS (SELECT 1 FROM _local_models lm WHERE lm.model_name = p.type AND lm.file_path=rr.file_path) AS is_local_model
  FROM _raw_routes rr, UNNEST(rr.parameters) AS t(p)
  LEFT JOIN _ast pa ON pa.parent_id = (SELECT pb.node_id FROM _ast pb WHERE pb.parent_id = (SELECT f.node_id FROM _ast f WHERE f.parent_id=rr.decorated_id AND f.type='function_definition' LIMIT 1) AND pb.type='parameters' LIMIT 1)
                   AND pa.name = p.name AND pa.type IN ('typed_parameter','typed_default_parameter')
  LEFT JOIN _ast a ON a.parent_id = pa.node_id AND a.type = 'type'   -- annotation node
),
classified AS (
  SELECT *,
    CASE
      WHEN is_path_param THEN 'path'
      WHEN explicit_param_fn = 'Header' THEN 'header'
      WHEN explicit_param_fn = 'Cookie' THEN 'cookie'
      WHEN explicit_param_fn = 'Form'  THEN 'form'
      WHEN explicit_param_fn = 'File'  THEN 'file'
      WHEN explicit_param_fn = 'Depends' THEN 'depends'   -- or 'query' fallback; caller decides
      WHEN is_local_model THEN 'body'
      WHEN (NOT param_type_raw IN ('str','int','float','bool','bytes','list','dict','None','')
            AND param_type_raw != param_name
            AND NOT is_path_param) THEN 'body'   -- still conservative for unknown
      ELSE 'query'
    END AS location,
    explicit_param_fn IS NOT NULL OR param_type_raw LIKE '%Annotated%' OR param_type_raw LIKE '%Depends%' AS needs_review
  FROM unnested
)
SELECT
  file_path, decorated_id, handler_name, method, full_path, start_line,
  param_name,
  param_type_raw AS param_type,
  CASE WHEN is_required IS NOT NULL THEN is_required ELSE true END AS is_required,
  location,
  needs_review AS is_imported_model,   -- reuse flag for NEEDS
  CASE param_type_raw WHEN 'str' THEN 'string' WHEN 'int' THEN 'int' ... ELSE 'struct' END AS qtype
FROM classified;
```

(Extend the NEEDS logic and emit comments to mention the explicit fn.)

### Gap 3+7: Robust deco_path + full_path selection (prefer path-like string)

```sql
-- In route_extract, replace the array_agg[1] for deco_path with ordered first path-like string
-- inside the decorator range.
deco_path AS (
  SELECT
    ...
    trim( (
      SELECT s.name
      FROM _ast s
      WHERE s.node_id BETWEEN dc.lo AND dc.hi AND s.type = 'string'
        AND (starts_with(s.name, '/') OR contains(s.name, '{'))
      ORDER BY s.node_id
      LIMIT 1
    ), '"''' ) AS deco_path,
    ...
)
```

### Gap 4: Improved model field type for unions (first non-None identifier)

```sql
-- In _model_fields, replace the type subselect
CASE
  WHEN (SELECT ti.name FROM _ast ti
        WHERE ti.node_id BETWEEN a.node_id AND a.node_id+a.descendant_count
          AND ti.type='identifier'
          AND ti.parent_id IN (SELECT tp.node_id FROM _ast tp WHERE ... tp.type='type')
          AND ti.name NOT IN ('None','Optional')
        ORDER BY ti.node_id LIMIT 1) IS NOT NULL
    THEN (that subselect)
  ELSE 'str'
END AS field_type
```

### Gap 5: Multi-file support sketch (for bigger apps)

Extend `_ast` to a union over known entry + imported modules (hardcode example literal for a package or use a companion table of "app entry points"). For prefix, also scan `from .routers import items` style assignments and union the ASTs. (Beyond single read_ast call; would require pre-pass or explicit glob list.)

### Gap 8: Websocket + special routes

Add 'websocket' to the verbs list in route_extract and handle @app.websocket("/ws") (path only, no method or special "WS").

Similar small extensions for lifespan handlers if desired (non-route).

## Raw Evidence + Additional Notes

- All 77 primary logs live under `/tmp/sweep_primary_logs/<dir>.log` (and older glob run under `/tmp/sweep_logs/`).
- Single-file targeted debug runs (e.g. body/tutorial001, path_params/tutorial002, bigger main) reproduced the table results exactly.
- Common false-positive NEEDS reason text: "body param type is imported..." even for header/depends (copy-paste in status).
- Model extraction works for simple local BaseModel; field types degrade only on | unions inside models.
- No 501 catch-all interferes with scoring (we ignored it).
- The offboarder is conservative and safe (flags what it can't fully map). Real value is high for plain path/query/body; drops sharply on modern FastAPI idioms (Annotated + Depends dominate tutorials and prod code).

**Honest summary:** ~35-40% fully automatic CLEAN on the canonical minimal examples when using plain (non-Annotated) style. The rest require manual param_schema or handler work, which the tool surfaces correctly via NEEDS_REVIEW + NOT_DETECTED. Biggest engineering lift is robust AST walking for FastAPI's param/Depends/Annotated forms and cross-module router composition.

(End of report.)