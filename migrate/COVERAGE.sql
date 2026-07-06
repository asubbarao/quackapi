-- ============================================================================
-- COVERAGE.sql — FastAPI → quackapi migration safety report
--
-- Run AFTER migrate_fastapi.sql has been run in the same session
-- (the temp tables _raw_routes, _route_status, _dynamic_routes, _mount_calls,
--  _cbv_routes, _local_models, _di_aliases, _router_var_names are all alive).
--
-- Taxonomy:
--   MIGRATED      — decorator-based routes with no imported model params,
--                   no dynamic paths, no inline Annotated params.
--                   Auto-generated stubs are complete (modulo writing SQL handler).
--   NEEDS_REVIEW  — decorator-based routes with at least one problem:
--                     • body param type is not found anywhere in the glob
--                     • route is a class-based view (CBV)
--                     • first positional arg is non-literal (dynamic path)
--                     • inline Annotated[T, Param()] params need manual check
--   NOT_DETECTED  — routes the decorator scan cannot see at all:
--                     • app.add_api_route(...) dynamic registrations
--                     • app.mount(...)         sub-application mounts
--
-- Every route from the source either appears in MIGRATED/NEEDS_REVIEW OR
-- is counted in NOT_DETECTED. Nothing silently vanishes.
-- ============================================================================

-- ── Section 0: Ground-truth self-check ───────────────────────────────────────
-- Count HTTP-verb decorators in the AST (the ground truth) and compare against
-- the number of routes emitted. A difference here means the tool missed or
-- double-counted something — investigate before shipping.
--
-- The ground-truth count uses the same router-var guard as the tool:
-- only decorators whose callee object is a known APIRouter/FastAPI variable
-- are counted (eliminates click/typer/ORM false positives).
SELECT
  '=== GROUND-TRUTH SELF-CHECK ===' AS section,
  '' AS detail

UNION ALL

SELECT 'AST decorator count (ground truth)',
  CAST((
    SELECT count(DISTINCT d.parent_id || '|' || d.file_path)
    FROM _ast d
    WHERE d.type = 'decorator'
      AND EXISTS (
        SELECT 1 FROM _ast cv
        WHERE cv.file_path = d.file_path
          AND cv.node_id BETWEEN d.node_id AND d.node_id + d.descendant_count
          AND cv.type = 'identifier'
          AND cv.name IN ('get','post','put','delete','patch','head','options','websocket')
      )
      AND EXISTS (
        SELECT 1 FROM _ast fn
        WHERE fn.file_path = d.file_path
          AND fn.parent_id = d.parent_id AND fn.type = 'function_definition'
      )
      -- Router-var guard: deco var must be a known APIRouter/FastAPI var
      AND EXISTS (
        SELECT 1 FROM _router_var_names rvn
        WHERE rvn.var_name = (
          SELECT id_a.name FROM _ast id_a
          WHERE id_a.file_path = d.file_path
            AND id_a.node_id BETWEEN d.node_id AND d.node_id + d.descendant_count
            AND id_a.type = 'identifier'
          ORDER BY id_a.node_id ASC LIMIT 1
        )
      )
  ) AS VARCHAR) || ' HTTP-verb decorators on function_definition nodes (router-var guarded)'

UNION ALL

SELECT 'Tool emitted routes',
  CAST((SELECT count(*) FROM _route_status) AS VARCHAR) ||
  ' routes (MIGRATED + NEEDS_REVIEW combined)'

UNION ALL

SELECT 'Delta (emitted - ground truth)',
  CAST(
    (SELECT count(*) FROM _route_status) -
    (SELECT count(DISTINCT d.parent_id || '|' || d.file_path)
     FROM _ast d
     WHERE d.type = 'decorator'
       AND EXISTS (
         SELECT 1 FROM _ast cv
         WHERE cv.file_path = d.file_path
           AND cv.node_id BETWEEN d.node_id AND d.node_id + d.descendant_count
           AND cv.type = 'identifier'
           AND cv.name IN ('get','post','put','delete','patch','head','options','websocket')
       )
       AND EXISTS (
         SELECT 1 FROM _ast fn
         WHERE fn.file_path = d.file_path
           AND fn.parent_id = d.parent_id AND fn.type = 'function_definition'
       )
       AND EXISTS (
         SELECT 1 FROM _router_var_names rvn
         WHERE rvn.var_name = (
           SELECT id_a.name FROM _ast id_a
           WHERE id_a.file_path = d.file_path
             AND id_a.node_id BETWEEN d.node_id AND d.node_id + d.descendant_count
             AND id_a.type = 'identifier'
           ORDER BY id_a.node_id ASC LIMIT 1
         )
       ))
  AS VARCHAR) || ' (0 = clean; positive = over-counted; negative = missed)';

-- ── Section 1: Summary table ─────────────────────────────────────────────────
SELECT
  '=== COVERAGE REPORT ===' AS section,
  '' AS detail

UNION ALL

SELECT 'MIGRATED',
  'Routes auto-generated (write SQL handlers): ' ||
  CAST((SELECT count(*) FROM _route_status WHERE status = 'MIGRATED') AS VARCHAR) ||
  ' routes'

UNION ALL

SELECT 'NEEDS_REVIEW',
  'Routes with problems (imported models, CBV, dynamic path, Annotated params): ' ||
  CAST(COALESCE((SELECT count(*) FROM _route_status WHERE status = 'NEEDS_REVIEW'), 0) AS VARCHAR) ||
  ' routes'

UNION ALL

SELECT 'NOT_DETECTED (add_api_route)',
  'Dynamic registrations invisible to decorator scan: ' ||
  CAST(COALESCE((SELECT count(*) FROM _dynamic_routes), 0) AS VARCHAR) ||
  ' calls'

UNION ALL

SELECT 'NOT_DETECTED (app.mount)',
  'Sub-application mounts invisible to decorator scan: ' ||
  CAST(COALESCE((SELECT count(*) FROM _mount_calls), 0) AS VARCHAR) ||
  ' calls'

UNION ALL

SELECT 'LOCAL MODELS',
  'BaseModel/SQLModel classes extractable by this tool: ' ||
  CAST(COALESCE((SELECT count(*) FROM _local_models), 0) AS VARCHAR) ||
  ' models'

UNION ALL

SELECT 'DI ALIASES',
  'Annotated[X, Depends(...)] aliases excluded from param_schema: ' ||
  CAST(COALESCE((SELECT count(*) FROM _di_aliases), 0) AS VARCHAR) ||
  ' aliases';

-- ── Section 2: MIGRATED route detail ─────────────────────────────────────────
SELECT
  '--- MIGRATED routes ---' AS status,
  method || ' ' || full_path AS route,
  handler_name,
  CASE WHEN response_model IS NOT NULL THEN 'response_model=' || response_model ELSE '' END AS response_model,
  file_path || ':' || start_line AS location
FROM _route_status
WHERE status = 'MIGRATED'
ORDER BY file_path, start_line;

-- ── Section 3: NEEDS_REVIEW route detail ─────────────────────────────────────
SELECT
  '--- NEEDS_REVIEW routes ---' AS status,
  method || ' ' || full_path AS route,
  handler_name,
  CASE
    WHEN is_cbv THEN 'CBV: method on a class body — not a plain function'
    WHEN has_dynamic_path THEN 'DYNAMIC PATH: first positional arg is not a string literal'
    WHEN has_imported_model THEN
      'IMPORTED MODEL: body param type not found in the glob — fields unknown'
    WHEN has_unsupported_constraint THEN
      'UNENFORCED CONSTRAINT(S): [' || COALESCE(unsupported_constraint_detail, '') ||
      '] — quackapi runtime enforces le/ge only; enforce the rest in handler SQL'
    WHEN has_inline_depends THEN
      'INLINE DEPENDS: Annotated[..., Depends(...)] — resolve the dependency manually'
    ELSE 'UNKNOWN reason'
  END AS reason,
  file_path || ':' || start_line AS location
FROM _route_status
WHERE status = 'NEEDS_REVIEW'
ORDER BY file_path, start_line;

-- ── Section 4: NOT_DETECTED — add_api_route calls ────────────────────────────
SELECT
  '--- NOT_DETECTED: add_api_route ---' AS status,
  path AS route,
  'dynamic registration' AS handler_name,
  'Decorator scan cannot see add_api_route() calls. Register manually.' AS reason,
  file_path || ':' || start_line AS location
FROM _dynamic_routes
ORDER BY file_path, start_line;

-- ── Section 5: NOT_DETECTED — app.mount calls ────────────────────────────────
SELECT
  '--- NOT_DETECTED: app.mount ---' AS status,
  mount_path AS route,
  'sub-application' AS handler_name,
  'app.mount() attaches a whole sub-app. Migrate the sub-app separately.' AS reason,
  file_path || ':' || start_line AS location
FROM _mount_calls
ORDER BY file_path, start_line;

-- ── Section 6: NEEDS_REVIEW param detail ─────────────────────────────────────
SELECT
  '--- NEEDS_REVIEW param detail ---' AS status,
  rp.handler_name || ' (' || rp.method || ' ' || rp.full_path || ')' AS route,
  rp.param_name,
  rp.param_type AS fastapi_type,
  rp.qtype AS quackapi_type,
  rp.location,
  CASE WHEN rp.is_required THEN 'required' ELSE 'optional' END AS required,
  CASE
    WHEN rp.is_imported_model AND rp.location != 'path'
      THEN 'IMPORTED MODEL — add param_schema rows manually after resolving type fields'
    WHEN rp.unsupported_constraints IS NOT NULL
      THEN 'UNENFORCED CONSTRAINT(S): ' || rp.unsupported_constraints || ' — enforce in handler SQL'
    WHEN rp.explicit_param_fn = 'Depends'
      THEN 'INLINE DEPENDS — resolve the dependency manually'
    WHEN rp.explicit_param_fn IS NOT NULL
      THEN 'ANNOTATED: ' || rp.explicit_param_fn || '() → ' || rp.location ||
           COALESCE(' constraint_json=' || rp.constraint_json, '')
    WHEN rp.is_untyped
      THEN 'UNTYPED — assumed query; verify if path param'
    ELSE 'OK'
  END AS flag
FROM _route_params rp
JOIN _route_status rs
  ON rs.decorated_id = rp.decorated_id AND rs.file_path = rp.file_path
WHERE rs.status = 'NEEDS_REVIEW'
ORDER BY rp.file_path, rp.start_line, rp.param_name;

-- ── Section 7: LOCAL model field summary ─────────────────────────────────────
SELECT
  '--- LOCAL Model fields (BaseModel/SQLModel) ---' AS status,
  mf.model_name AS route,
  mf.field_name,
  mf.field_type,
  '' AS fastapi_type,
  '' AS quackapi_type,
  '' AS location,
  CASE WHEN mf.has_default THEN 'optional' ELSE 'required' END AS required,
  'AUTO-EXTRACTABLE' AS flag
FROM _model_fields mf
ORDER BY mf.model_name, mf.field_name;

-- ── Section 8: DI alias summary ───────────────────────────────────────────────
SELECT
  '--- DI Aliases (excluded from param_schema) ---' AS status,
  alias_name AS route,
  '' AS handler_name,
  'Annotated[X, Depends(...)] — pure DI wiring, no request payload' AS reason,
  '' AS location
FROM _di_aliases
ORDER BY alias_name;
