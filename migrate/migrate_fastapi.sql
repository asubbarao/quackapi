-- ============================================================================
-- migrate_fastapi.sql — FastAPI → quackapi migration extractor
--
-- The source path is supplied by the caller via the QUACKAPI_SRC environment
-- variable, read at query time via getenv('QUACKAPI_SRC').  No session
-- variables (SET VARIABLE / getvariable()) are used anywhere in this file.
--
-- Typical invocation via run_migrate.sh:
--   bash migrate/run_migrate.sh 'path/to/repo/**/*.py'
--
-- Manual invocation:
--   QUACKAPI_SRC='path/to/repo/**/*.py' \
--     /opt/homebrew/bin/duckdb -unsigned /tmp/qmig.db \
--     -c ".read migrate/migrate_fastapi.sql" \
--     -c ".read migrate/COVERAGE.sql"
--
-- What it produces:
--   • register_route INSERTs for each extracted route
--   • param_schema INSERTs for each classified parameter
--   • Model field reference as SQL comments
--   • A 501 catch-all route for paths not yet migrated
--
-- What it does NOT do (explicit boundary):
--   • Does NOT translate handler bodies. The handler column is a TODO placeholder.
--     You write the DuckDB SQL handler that executes per request.
--   • Does NOT resolve imported models whose source file is outside the glob.
--     Body params whose type is not found anywhere in the glob get type='struct'
--     and a NEEDS_REVIEW comment.
--   • Does NOT handle add_api_route() or class-based views — those appear in
--     COVERAGE.sql as NOT_DETECTED / NEEDS_REVIEW.
--   • Does NOT migrate sub-application routes (app.mount sub-apps are separate
--     FastAPI instances; migrate them independently).
--
-- IMPORTANT — multi-file AST node IDs restart at 0 per file.
-- Every containment query of the form:
--   arg.node_id BETWEEN parent.node_id AND parent.node_id + parent.descendant_count
-- MUST also include:
--   arg.file_path = parent.file_path
-- Without the file_path guard the ranges overlap across files and produce
-- false positives.
-- ============================================================================

INSTALL sitting_duck FROM community; LOAD sitting_duck;

-- ── Step 1: read AST ─────────────────────────────────────────────────────────
CREATE OR REPLACE TEMP TABLE _ast AS
SELECT * FROM read_ast(getenv('QUACKAPI_SRC'), 'python');

-- ── Step 2a: direct model base classes (BaseModel or SQLModel) ───────────────
-- class_definition at module depth (depth=1) whose argument_list directly
-- contains an identifier named 'BaseModel' or 'SQLModel'.
CREATE OR REPLACE TEMP TABLE _direct_models AS
SELECT
  cd.node_id    AS class_id,
  cd.name       AS model_name,
  cd.file_path  AS file_path,
  cd.start_line AS start_line
FROM _ast cd
WHERE cd.type = 'class_definition'
  AND cd.depth = 1
  AND EXISTS (
    SELECT 1 FROM _ast arg
    WHERE arg.file_path = cd.file_path
      AND arg.node_id BETWEEN cd.node_id AND cd.node_id + cd.descendant_count
      AND arg.type = 'identifier'
      AND arg.name IN ('BaseModel', 'SQLModel')
      AND arg.parent_id != cd.node_id
  );

-- ── Step 2b: transitive local models (inherit from a direct model) ────────────
-- One level of indirection: class X(Y) where Y is already in _direct_models.
-- Covers patterns like UserCreate(UserBase), ItemPublic(ItemBase).
CREATE OR REPLACE TEMP TABLE _local_models AS
SELECT class_id, model_name, file_path, start_line FROM _direct_models
UNION
SELECT
  cd.node_id    AS class_id,
  cd.name       AS model_name,
  cd.file_path  AS file_path,
  cd.start_line AS start_line
FROM _ast cd
WHERE cd.type = 'class_definition'
  AND cd.depth = 1
  AND EXISTS (
    SELECT 1 FROM _ast arg
    WHERE arg.file_path = cd.file_path
      AND arg.node_id BETWEEN cd.node_id AND cd.node_id + cd.descendant_count
      AND arg.type = 'identifier'
      AND EXISTS (
        SELECT 1 FROM _direct_models dm WHERE dm.model_name = arg.name
      )
      AND arg.parent_id != cd.node_id
  );

-- ── Step 3: model field extraction ───────────────────────────────────────────
-- assignment nodes inside the class body carry name + type annotation.
-- A '=' sibling means a default value is present → field is optional.
-- First non-None identifier in the type annotation handles T | None unions.
CREATE OR REPLACE TEMP TABLE _model_fields AS
SELECT
  m.model_name,
  m.file_path,
  a.name AS field_name,
  CASE
    WHEN (SELECT ti.name
          FROM _ast ti
          WHERE ti.file_path = a.file_path
            AND ti.node_id BETWEEN a.node_id AND a.node_id + a.descendant_count
            AND ti.type = 'identifier'
            AND ti.name NOT IN ('None', 'Optional')
            AND ti.parent_id IN (
              SELECT tp.node_id FROM _ast tp
              WHERE tp.file_path = a.file_path
                AND tp.node_id BETWEEN a.node_id AND a.node_id + a.descendant_count
                AND tp.type = 'type'
            )
          ORDER BY ti.node_id LIMIT 1) IS NOT NULL
      THEN (SELECT ti.name
            FROM _ast ti
            WHERE ti.file_path = a.file_path
              AND ti.node_id BETWEEN a.node_id AND a.node_id + a.descendant_count
              AND ti.type = 'identifier'
              AND ti.name NOT IN ('None', 'Optional')
              AND ti.parent_id IN (
                SELECT tp.node_id FROM _ast tp
                WHERE tp.file_path = a.file_path
                  AND tp.node_id BETWEEN a.node_id AND a.node_id + a.descendant_count
                  AND tp.type = 'type'
              )
            ORDER BY ti.node_id LIMIT 1)
    ELSE 'str'
  END AS field_type,
  EXISTS (
    SELECT 1 FROM _ast eq
    WHERE eq.parent_id = a.node_id AND eq.type = '='
  ) AS has_default
FROM _local_models m
JOIN _ast blk ON blk.parent_id = m.class_id AND blk.type = 'block'
              AND blk.file_path = m.file_path
JOIN _ast es  ON es.parent_id  = blk.node_id AND es.type  = 'expression_statement'
              AND es.file_path = m.file_path
JOIN _ast a   ON a.parent_id   = es.node_id  AND a.type   = 'assignment'
              AND a.file_path  = m.file_path
WHERE a.name IS NOT NULL;

-- ── Step 4: dependency-injection (DI) type aliases ───────────────────────────
-- Detect module-level assignments of the form:
--   SomeAlias = Annotated[SomeType, Depends(...)]
-- These are pure DI wiring; params typed with these names are NOT data params
-- and must be excluded from param_schema (they carry no request payload).
--
-- Pattern: assignment node at depth=2 (inside expression_statement at depth=1)
-- whose RHS is a subscript whose first child identifier is 'Annotated' AND
-- whose descendant list contains a call named 'Depends'.
CREATE OR REPLACE TEMP TABLE _di_aliases AS
SELECT DISTINCT asgn.name AS alias_name
FROM _ast asgn
WHERE asgn.type = 'assignment'
  AND asgn.depth = 2
  AND asgn.name IS NOT NULL
  AND EXISTS (
    SELECT 1 FROM _ast sub
    WHERE sub.file_path = asgn.file_path
      AND sub.node_id BETWEEN asgn.node_id AND asgn.node_id + asgn.descendant_count
      AND sub.type = 'subscript'
      AND EXISTS (
        SELECT 1 FROM _ast aid
        WHERE aid.file_path = asgn.file_path
          AND aid.node_id BETWEEN sub.node_id AND sub.node_id + sub.descendant_count
          AND aid.type = 'identifier'
          AND aid.name = 'Annotated'
          AND aid.parent_id = sub.node_id
      )
      AND EXISTS (
        SELECT 1 FROM _ast dep
        WHERE dep.file_path = asgn.file_path
          AND dep.node_id BETWEEN sub.node_id AND sub.node_id + sub.descendant_count
          AND dep.type = 'call'
          AND dep.name = 'Depends'
      )
  );

-- ── Step 4b: FastAPI special-injection types to suppress from param_schema ────
-- BackgroundTasks, Request, Response — injected by FastAPI at call time,
-- carry no request payload and must not appear in param_schema.
-- These are NOT DI aliases (no Annotated+Depends pattern) but are always excluded.
CREATE OR REPLACE TEMP TABLE _fastapi_injected AS
SELECT alias_name FROM (
  VALUES
    ('BackgroundTasks'),
    ('Request'),
    ('Response'),
    ('HTTPConnection'),
    ('WebSocket')
) AS t(alias_name);

-- ── Step 5a: router var table — only treat @X.verb() as a route when X is
--    assigned from APIRouter() or FastAPI() anywhere in the scanned glob.
--    Key: (var_name, file_path).  Also maintain a global name set for
--    cross-file matching (dispatch decorates in views.py using routers defined
--    in the same file; include_router wiring is in api.py).
CREATE OR REPLACE TEMP TABLE _router_vars AS
SELECT DISTINCT asgn.name AS var_name, asgn.file_path
FROM _ast asgn
WHERE asgn.type = 'assignment'
  AND EXISTS (
    SELECT 1 FROM _ast c
    WHERE c.file_path = asgn.file_path
      AND c.parent_id = asgn.node_id
      AND c.type = 'call'
      AND c.name IN ('APIRouter', 'FastAPI', 'Starlette')
  );

-- ── Step 5b: router own-prefix from APIRouter(prefix=...) ────────────────────
-- var = APIRouter(prefix="/v1") → var_name='var', own_prefix='/v1'
CREATE OR REPLACE TEMP TABLE _router_own_prefix AS
SELECT
  asgn.name        AS var_name,
  asgn.file_path   AS file_path,
  CASE
    WHEN (
      SELECT pstr.name
      FROM _ast kw
      JOIN _ast pstr ON pstr.file_path = kw.file_path
                     AND pstr.node_id BETWEEN kw.node_id AND kw.node_id + kw.descendant_count
                     AND pstr.type = 'string'
      WHERE kw.file_path = asgn.file_path
        AND kw.node_id BETWEEN asgn.node_id AND asgn.node_id + asgn.descendant_count
        AND kw.type = 'keyword_argument'
        AND kw.name = 'prefix'
      LIMIT 1
    ) IS NOT NULL
      THEN trim(
        (SELECT pstr.name
         FROM _ast kw
         JOIN _ast pstr ON pstr.file_path = kw.file_path
                        AND pstr.node_id BETWEEN kw.node_id AND kw.node_id + kw.descendant_count
                        AND pstr.type = 'string'
         WHERE kw.file_path = asgn.file_path
           AND kw.node_id BETWEEN asgn.node_id AND asgn.node_id + asgn.descendant_count
           AND kw.type = 'keyword_argument'
           AND kw.name = 'prefix'
         LIMIT 1),
        '"'''
      )
    ELSE ''
  END AS own_prefix
FROM _ast asgn
WHERE asgn.type = 'assignment'
  AND EXISTS (
    SELECT 1 FROM _ast c
    WHERE c.file_path = asgn.file_path
      AND c.parent_id = asgn.node_id
      AND c.type = 'call'
      AND c.name IN ('APIRouter', 'FastAPI', 'Starlette')
  );

-- ── Step 5b.5: cross-file import alias resolution ────────────────────────────
-- Resolve "from dispatch.X.views import router as X_router" statements:
--   alias_name    = X_router (the name used in include_router calls in api.py)
--   original_name = router (the name used as @router.get() in views.py)
--   resolved_file = /path/to/dispatch/X/views.py (the file defining the router)
--
-- Filters applied to avoid false matches:
--   1. The import_from_statement's module path must resolve (via '.' → '/') to a
--      real file in the glob that has `original_name` in _router_own_prefix.
--   2. The alias_name must actually be used in an include_router() call in the
--      importer file (not just imported for other purposes).
--   3. DISTINCT ON (alias_name) with longest-module-path tie-break for ambiguous cases.
CREATE OR REPLACE TEMP TABLE _router_import_aliases AS
WITH all_files AS (SELECT DISTINCT file_path FROM _ast)
SELECT DISTINCT ON (ai.name)
  ai.name             AS alias_name,         -- e.g. ai_router (in api.py)
  dn.name             AS original_name,      -- e.g. router (in views.py)
  ifs.file_path       AS importer_file_path, -- api.py
  af.file_path        AS resolved_file_path  -- /path/to/ai/prompt/views.py
FROM _ast ifs
JOIN _ast ai ON ai.parent_id = ifs.node_id AND ai.type = 'aliased_import'
JOIN _ast dn ON dn.parent_id = ai.node_id AND dn.type = 'dotted_name'
-- Resolve module path to file path: 'dispatch.ai.prompt.views' → '.../dispatch/ai/prompt/views.py'
JOIN all_files af ON ifs.name IS NOT NULL
                  AND ifs.name != ''
                  AND ends_with(af.file_path, '/' || replace(ifs.name, '.', '/') || '.py')
WHERE ifs.type = 'import_from_statement'
  -- original_name must be a real router var in the resolved file (not just any 'router' import)
  AND EXISTS (SELECT 1 FROM _router_own_prefix op
              WHERE op.var_name = dn.name AND op.file_path = af.file_path)
  -- alias_name must appear in an include_router() call in the importer file
  AND EXISTS (
    SELECT 1 FROM _ast c
    WHERE c.file_path = ifs.file_path AND c.type = 'call' AND c.name = 'include_router'
      AND EXISTS (
        SELECT 1 FROM _ast args
        WHERE args.file_path = c.file_path AND args.parent_id = c.node_id
          AND args.type = 'argument_list'
          AND EXISTS (
            SELECT 1 FROM _ast id_n
            WHERE id_n.file_path = args.file_path
              AND id_n.parent_id = args.node_id
              AND id_n.type = 'identifier' AND id_n.name = ai.name
          )
      )
  )
ORDER BY ai.name, length(ifs.name) DESC;  -- longest module path wins if ambiguous

-- Extend _router_var_names to include import aliases (so decorated_definition
-- guard accepts alias vars as valid router vars)
CREATE OR REPLACE TEMP TABLE _router_var_names AS
SELECT DISTINCT var_name FROM _router_vars
UNION
SELECT DISTINCT alias_name AS var_name FROM _router_import_aliases;

-- ── Step 5c: include_router prefix — CROSS-FILE ───────────────────────────────
-- Model `parent.include_router(child_router_var, prefix='/x')` calls across ALL files.
-- The first identifier in argument_list is the router variable name being included.
-- We extract the literal prefix= kwarg if present.
-- For non-literal prefix (e.g. settings.API_V1_STR), we mark as '<dynamic:prefix>'.
-- Cross-file: the var_name here is looked up globally in _router_var_names.
CREATE OR REPLACE TEMP TABLE _include_extra_prefix AS
SELECT
  -- first identifier in the argument_list = the router var being included
  (SELECT id_n.name
   FROM _ast id_n
   WHERE id_n.file_path = args.file_path
     AND id_n.parent_id = args.node_id
     AND id_n.type = 'identifier'
     AND id_n.node_id = (
       SELECT min(x.node_id) FROM _ast x
       WHERE x.file_path = args.file_path
         AND x.parent_id = args.node_id AND x.type = 'identifier'
     )
   LIMIT 1) AS var_name,
  ir.file_path AS caller_file_path,
  CASE
    -- check if there is a literal string in the prefix= kwarg
    WHEN (
      SELECT pstr.name
      FROM _ast kw2
      JOIN _ast pstr ON pstr.file_path = kw2.file_path
                     AND pstr.node_id BETWEEN kw2.node_id AND kw2.node_id + kw2.descendant_count
                     AND pstr.type = 'string'
      WHERE kw2.file_path = ir.file_path
        AND kw2.node_id BETWEEN args.node_id AND args.node_id + args.descendant_count
        AND kw2.type = 'keyword_argument'
        AND kw2.name = 'prefix'
      LIMIT 1
    ) IS NOT NULL
      THEN trim(
        (SELECT pstr.name
         FROM _ast kw2
         JOIN _ast pstr ON pstr.file_path = kw2.file_path
                        AND pstr.node_id BETWEEN kw2.node_id AND kw2.node_id + kw2.descendant_count
                        AND pstr.type = 'string'
         WHERE kw2.file_path = ir.file_path
           AND kw2.node_id BETWEEN args.node_id AND args.node_id + args.descendant_count
           AND kw2.type = 'keyword_argument'
           AND kw2.name = 'prefix'
         LIMIT 1),
        '"'''
      )
    -- prefix= kwarg exists but has no string literal → dynamic
    WHEN EXISTS (
      SELECT 1 FROM _ast kw3
      WHERE kw3.file_path = ir.file_path
        AND kw3.node_id BETWEEN args.node_id AND args.node_id + args.descendant_count
        AND kw3.type = 'keyword_argument'
        AND kw3.name = 'prefix'
    ) THEN '<dynamic:prefix>'
    ELSE ''
  END AS extra_prefix
FROM _ast ir
JOIN _ast args ON args.file_path = ir.file_path
              AND args.parent_id = ir.node_id AND args.type = 'argument_list'
WHERE ir.type = 'call'
  AND ir.name = 'include_router';

-- ── Step 5d: combined prefix per router variable ──────────────────────────────
-- own_prefix || extra_prefix. Routers with no APIRouter assignment get only
-- the include_router prefix. Handles /items + '' = /items and '' + /users = /users.
--
-- Cross-file alias resolution:
--   Case A — same-file: router var defined and decorated in same file, prefix from
--     include_router in same file (standard single-file case).
--   Case B — cross-file import alias:
--     views.py defines `router = APIRouter()` and decorates with @router.get()
--     api.py does `from dispatch.X.views import router as X_router`
--     api.py does `include_router(X_router, prefix='/x')`
--     → routes in views.py using `router` get prefix '/x' from api.py.
--     The _router_import_aliases table resolves: alias_name → resolved_file_path.
--     We join: _include_extra_prefix (var_name=alias_name) →
--              _router_import_aliases (alias_name→resolved_file_path, original_name) →
--              _router_own_prefix (var_name=original_name, file_path=resolved_file_path).
--   Case C — direct extra_prefix only (no own_prefix assignment found).
CREATE OR REPLACE TEMP TABLE _router_prefix AS
-- Case A: direct match (same file, var_name in own_prefix)
SELECT
  op.var_name,
  op.file_path,
  op.own_prefix || CASE WHEN ep.extra_prefix IS NULL THEN '' ELSE ep.extra_prefix END AS prefix
FROM _router_own_prefix op
LEFT JOIN _include_extra_prefix ep ON ep.var_name = op.var_name
-- Don't apply direct extra_prefix if this var is the original_name behind an alias
-- (Case B handles it more precisely)
WHERE NOT EXISTS (
  SELECT 1 FROM _router_import_aliases ria
  WHERE ria.original_name = op.var_name
    AND ria.resolved_file_path = op.file_path
)
UNION ALL
-- Case B: cross-file import alias — routes decorated with `router` in views.py
--   get the prefix from include_router(X_router, prefix='/x') in api.py.
SELECT
  op.var_name,              -- original_name (e.g. 'router') in views.py
  op.file_path,             -- views.py file path
  op.own_prefix || ep.extra_prefix AS prefix
FROM _router_own_prefix op
JOIN _router_import_aliases ria ON ria.original_name = op.var_name
                                AND ria.resolved_file_path = op.file_path
JOIN _include_extra_prefix ep ON ep.var_name = ria.alias_name
UNION ALL
-- Case C: include_router var has no own_prefix (not in _router_own_prefix),
--   and is not an alias for something else.
SELECT ep.var_name, ep.caller_file_path AS file_path, ep.extra_prefix AS prefix
FROM _include_extra_prefix ep
WHERE NOT EXISTS (
  SELECT 1 FROM _router_own_prefix op WHERE op.var_name = ep.var_name
)
  AND NOT EXISTS (
  SELECT 1 FROM _router_import_aliases ria WHERE ria.alias_name = ep.var_name
);

-- ── Step 6: raw route extraction ─────────────────────────────────────────────
-- Scan every decorator node; extract HTTP verb, path string, decorator variable,
-- response_model (if present), and function_definition sibling.
--
-- Fix #1 — Precise path extraction:
--   Path = the string whose parent_id is the argument_list of the HTTP-verb call
--   node AND whose node_id is the minimum (first by source order) among all
--   string children of that argument_list.  This is the FIRST POSITIONAL ARGUMENT.
--   Non-literal path args (expressions, identifiers, f-strings) → deco_path=NULL
--   → routed to NEEDS_REVIEW with reason 'dynamic path'.
--
-- Fix #2 — Router-var guard:
--   @X.verb() is only treated as a route when X appears in _router_var_names
--   (i.e. X was assigned from APIRouter() or FastAPI() somewhere in the glob).
--   This eliminates click/typer/ORM client .get() false positives.
--
-- Fix #3 — Dedup:
--   DISTINCT on (file_path, decorated_id, method) in _route_status to prevent
--   multi-decorator functions from emitting the same route twice.
--
-- Fix #8 — WebSocket verb:
--   'websocket' added to the verbs list; method emitted as 'WS'.
CREATE OR REPLACE TEMP TABLE _raw_routes AS
WITH
verbs AS (SELECT ['get','post','put','delete','patch','head','options','websocket'] AS v),
deco AS (
  SELECT d.file_path, d.parent_id AS decorated_id,
    d.node_id AS lo, d.node_id + d.descendant_count AS hi, d.start_line
  FROM _ast d WHERE d.type = 'decorator'
),
route_extract AS (
  SELECT
    dc.file_path, dc.decorated_id, dc.start_line,
    -- Method: from the attribute node name (e.g. router.get → 'get')
    -- We look for an identifier inside the decorator subtree whose name is in verbs,
    -- selecting the LAST such identifier by node_id (the attribute name, not the object).
    -- For @router.get: identifiers are [router, get] — we want 'get' (max node_id).
    -- Using UPPER() so 'get' → 'GET', 'websocket' → 'WEBSOCKET' (remapped below).
    CASE
      WHEN (SELECT i.name
            FROM _ast i, verbs
            WHERE i.file_path = dc.file_path
              AND i.node_id BETWEEN dc.lo AND dc.hi
              AND i.type = 'identifier'
              AND array_length(array_intersect([i.name], verbs.v)) > 0
            ORDER BY i.node_id DESC LIMIT 1) = 'websocket'
        THEN 'WS'
      ELSE upper(
        (SELECT i.name
         FROM _ast i, verbs
         WHERE i.file_path = dc.file_path
           AND i.node_id BETWEEN dc.lo AND dc.hi
           AND i.type = 'identifier'
           AND array_length(array_intersect([i.name], verbs.v)) > 0
         ORDER BY i.node_id DESC LIMIT 1)
      )
    END AS method,
    -- Deco var: the FIRST identifier in the decorator subtree = the object being
    -- called (e.g. 'router' in @router.get).  ORDER BY node_id ASC.
    (SELECT id_a.name
     FROM _ast id_a
     WHERE id_a.file_path = dc.file_path
       AND id_a.node_id BETWEEN dc.lo AND dc.hi
       AND id_a.type = 'identifier'
     ORDER BY id_a.node_id ASC LIMIT 1) AS deco_var,
    -- Path: FIRST positional string argument inside the call's argument_list.
    -- We locate the call node (child of decorator with type='call'), then its
    -- argument_list child, then find the string node whose parent_id is that
    -- argument_list (direct child, not deeper) with minimum node_id.
    -- This avoids picking up strings from response_model=, tags=[], etc.
    (SELECT trim(s.name, '"''')
     FROM _ast call_node
     JOIN _ast arglist ON arglist.file_path = call_node.file_path
                       AND arglist.parent_id = call_node.node_id
                       AND arglist.type = 'argument_list'
     JOIN _ast s ON s.file_path = arglist.file_path
                 AND s.parent_id = arglist.node_id
                 AND s.type = 'string'
     WHERE call_node.file_path = dc.file_path
       AND call_node.parent_id = dc.lo   -- call is direct child of decorator
       AND call_node.type = 'call'
     ORDER BY s.node_id ASC LIMIT 1) AS deco_path,
    -- Is the first positional arg a non-literal (attribute, identifier, f-string)?
    -- If no string in argument_list but there IS an argument → dynamic path.
    EXISTS (
      SELECT 1
      FROM _ast call_node
      JOIN _ast arglist ON arglist.file_path = call_node.file_path
                        AND arglist.parent_id = call_node.node_id
                        AND arglist.type = 'argument_list'
      -- Check: has children that are NOT string, NOT keyword_argument, NOT punctuation
      JOIN _ast first_arg ON first_arg.file_path = arglist.file_path
                          AND first_arg.parent_id = arglist.node_id
                          AND first_arg.type NOT IN ('(', ')', ',', 'string', 'keyword_argument')
                          AND first_arg.node_id = (
                            SELECT min(x.node_id) FROM _ast x
                            WHERE x.file_path = arglist.file_path
                              AND x.parent_id = arglist.node_id
                              AND x.type NOT IN ('(', ')', ',')
                          )
      WHERE call_node.file_path = dc.file_path
        AND call_node.parent_id = dc.lo
        AND call_node.type = 'call'
      -- No string child at root level of arglist
      AND NOT EXISTS (
        SELECT 1 FROM _ast s2
        WHERE s2.file_path = arglist.file_path
          AND s2.parent_id = arglist.node_id
          AND s2.type = 'string'
      )
    ) AS has_dynamic_path,
    -- response_model: keyword_argument named 'response_model' inside the decorator.
    (SELECT rm_id.name
     FROM _ast kw_rm
     JOIN _ast rm_id ON rm_id.file_path = kw_rm.file_path
                     AND rm_id.node_id BETWEEN kw_rm.node_id AND kw_rm.node_id + kw_rm.descendant_count
                     AND rm_id.type = 'identifier'
                     AND rm_id.name != 'response_model'
     WHERE kw_rm.file_path = dc.file_path
       AND kw_rm.node_id BETWEEN dc.lo AND dc.hi
       AND kw_rm.type = 'keyword_argument'
       AND kw_rm.name = 'response_model'
     LIMIT 1) AS response_model
  FROM deco dc
)
SELECT
  re.file_path,
  re.decorated_id,
  re.start_line,
  re.method,
  COALESCE(re.deco_path, CASE WHEN re.has_dynamic_path THEN '<dynamic:path>' ELSE NULL END) AS deco_path,
  re.has_dynamic_path,
  re.deco_var,
  re.response_model,
  CASE WHEN rp.prefix IS NULL OR rp.prefix = '' THEN
    COALESCE(re.deco_path, CASE WHEN re.has_dynamic_path THEN '<dynamic:path>' ELSE NULL END)
  WHEN ends_with(rp.prefix, '/') AND starts_with(
    COALESCE(re.deco_path, CASE WHEN re.has_dynamic_path THEN '<dynamic:path>' ELSE NULL END), '/')
    THEN rp.prefix || substr(
      COALESCE(re.deco_path, CASE WHEN re.has_dynamic_path THEN '<dynamic:path>' ELSE NULL END), 2)
  ELSE rp.prefix ||
    COALESCE(re.deco_path, CASE WHEN re.has_dynamic_path THEN '<dynamic:path>' ELSE NULL END)
  END AS full_path,
  f.name       AS handler_name,
  f.parameters AS parameters,
  f.start_line AS func_start_line
FROM route_extract re
-- Fix #2: router-var guard — deco_var must be in _router_var_names
JOIN _router_var_names rvn ON rvn.var_name = re.deco_var
LEFT JOIN _router_prefix rp ON rp.var_name = re.deco_var
                           AND rp.file_path = re.file_path
-- Join on both var_name AND file_path: _router_prefix Case B stores file_path=views.py
-- (the file where the routes live), so this correctly scopes each route to its own prefix.
JOIN _ast f ON f.file_path = re.file_path
           AND f.parent_id = re.decorated_id AND f.type = 'function_definition'
-- method must be found; path must be non-null (either literal or dynamic placeholder)
WHERE re.method IS NOT NULL
  AND (re.deco_path IS NOT NULL OR re.has_dynamic_path);

-- ── Step 7: parameter classification ─────────────────────────────────────────
-- For each parameter struct {name, type}:
--   path   — name appears as {name} in a path segment of full_path
--   header — param annotated with Header() (inline Annotated[T, Header(...)])
--   cookie — param annotated with Cookie()
--   form   — param annotated with Form()
--   file   — param annotated with File() / UploadFile
--   body   — param type matches a local model name (across all files in the glob)
--   query  — everything else (primitives, unknown types)
--
-- Fix #5: Annotated + explicit param functions:
--   Walk the parameter's annotation subtree for Header/Cookie/Form/File/Query/Path/Depends.
--   For inline Annotated[T, Foo(...)], classify by Foo's name.
--   Depends → exclude (same as DI alias).
--
-- Fix #7: Untyped params:
--   typed_parameter with no type annotation AND name matches {name} in path → path param.
--   Untyped non-path param → query with NEEDS_REVIEW 'untyped param'.
--
-- DI alias params (SessionDep, CurrentUser, etc.) are excluded entirely.
-- FastAPI-injected types (BackgroundTasks, Request, Response) are excluded.
-- required: typed_parameter node = true; typed_default_parameter = false.
CREATE OR REPLACE TEMP TABLE _route_params AS
WITH unnested AS (
  SELECT
    rr.file_path,
    rr.decorated_id,
    rr.handler_name,
    rr.method,
    rr.full_path,
    rr.start_line,
    p.name AS param_name,
    p.type AS param_type,
    -- effective_type: for union types like `str | None`, extract the non-None base type.
    -- Handles `T | None`, `None | T`, and plain types alike.
    CASE
      WHEN p.type IS NULL OR NOT contains(p.type, ' | ') THEN p.type
      ELSE (
        -- split on ' | ', filter out 'None', take first non-None element
        list_filter(string_split(p.type, ' | '), lambda t: t != 'None')[1]
      )
    END AS effective_type,
    -- required: typed_parameter = true; typed_default_parameter = false — UNLESS
    -- the default is ellipsis-marked (`= Query(...)`, `= ...`), which FastAPI
    -- defines as REQUIRED despite occupying the default position.
    (SELECT CASE
       WHEN a2.type = 'typed_parameter' THEN true
       WHEN EXISTS (SELECT 1 FROM _ast el
                    WHERE el.file_path = a2.file_path
                      AND el.node_id BETWEEN a2.node_id AND a2.node_id + a2.descendant_count
                      AND el.type = 'ellipsis') THEN true
       ELSE false END
     FROM _ast fn
     JOIN _ast pblock ON pblock.file_path = fn.file_path
                     AND pblock.parent_id = fn.node_id AND pblock.type = 'parameters'
     JOIN _ast a2 ON a2.file_path = pblock.file_path
                  AND a2.parent_id = pblock.node_id
                  AND a2.name = p.name
                  AND a2.type IN ('typed_parameter', 'typed_default_parameter')
     WHERE fn.file_path = rr.file_path
       AND fn.parent_id = rr.decorated_id AND fn.type = 'function_definition'
     LIMIT 1) AS is_required,
    -- path param: {name} appears literally in a path segment
    array_length(list_filter(
      string_split(rr.full_path, '/'),
      lambda seg: seg = '{' || p.name || '}'
    )) > 0 AS is_path_param,
    -- DI alias: param type is a known Annotated[X, Depends(...)] alias
    EXISTS (
      SELECT 1 FROM _di_aliases da WHERE da.alias_name = effective_type
    ) AS is_di_alias,
    -- FastAPI-injected: BackgroundTasks, Request, Response, etc.
    EXISTS (
      SELECT 1 FROM _fastapi_injected fi WHERE fi.alias_name = effective_type
    ) AS is_fastapi_injected,
    -- local model: type matches a model class found anywhere in the glob
    EXISTS (
      SELECT 1 FROM _local_models lm WHERE lm.model_name = effective_type
    ) AS is_local_model,
    -- Fix #5 (widened by Fix #11): detect the FastAPI param function attached to
    -- this parameter. Searches the WHOLE typed_parameter subtree — annotation AND
    -- default position — so both `Annotated[T, Query(...)]` and the legacy
    -- `q: T = Query(...)` form classify identically (FastAPI treats them the same).
    (SELECT c.name
     FROM _ast fn
     JOIN _ast pblock ON pblock.file_path = fn.file_path
                     AND pblock.parent_id = fn.node_id AND pblock.type = 'parameters'
     JOIN _ast tp ON tp.file_path = pblock.file_path
                 AND tp.parent_id = pblock.node_id AND tp.name = p.name
                 AND tp.type IN ('typed_parameter', 'typed_default_parameter')
     JOIN _ast c ON c.file_path = tp.file_path
                AND c.node_id BETWEEN tp.node_id AND tp.node_id + tp.descendant_count
                AND c.type = 'call'
                AND c.name IN ('Header','Cookie','Form','File','Query','Path','Depends')
     WHERE fn.file_path = rr.file_path
       AND fn.parent_id = rr.decorated_id AND fn.type = 'function_definition'
     ORDER BY c.node_id ASC LIMIT 1) AS explicit_param_fn,
    -- Fix #11 (Annotated constraint pull-through): le/ge kwargs with direct
    -- integer/float literal values become quackapi constraint_json — these are
    -- exactly the constraints the runtime enforces (framework.sql
    -- less_than_equal / greater_than_equal). Negative literals hide behind an
    -- anonymous unary_operator node and are deliberately NOT extracted — they
    -- land in unsupported_constraints below instead of being guessed.
    (SELECT '{' || string_agg('"' || kw.name || '":' || v.name, ',' ORDER BY kw.node_id) || '}'
     FROM _ast fn
     JOIN _ast pblock ON pblock.file_path = fn.file_path
                     AND pblock.parent_id = fn.node_id AND pblock.type = 'parameters'
     JOIN _ast tp ON tp.file_path = pblock.file_path
                 AND tp.parent_id = pblock.node_id AND tp.name = p.name
                 AND tp.type IN ('typed_parameter', 'typed_default_parameter')
     JOIN _ast c ON c.file_path = tp.file_path
                AND c.node_id BETWEEN tp.node_id AND tp.node_id + tp.descendant_count
                AND c.type = 'call'
                AND c.name IN ('Header','Cookie','Form','File','Query','Path')
     JOIN _ast kw ON kw.file_path = c.file_path
                 AND kw.node_id BETWEEN c.node_id AND c.node_id + c.descendant_count
                 AND kw.type = 'keyword_argument'
                 AND kw.name IN ('le', 'ge')
     JOIN _ast v ON v.file_path = kw.file_path AND v.parent_id = kw.node_id
                AND v.type IN ('integer', 'float')
     WHERE fn.file_path = rr.file_path
       AND fn.parent_id = rr.decorated_id AND fn.type = 'function_definition'
    ) AS constraint_json,
    -- Constraint-family kwargs the runtime does NOT enforce (or whose value we
    -- cannot extract as a plain literal). A MIGRATED route that silently drops
    -- one of these would validate WEAKER than the FastAPI original, so their
    -- presence forces NEEDS_REVIEW with the kwargs named. `alias` is included:
    -- it renames the wire param, so ignoring it mis-names the parameter.
    (SELECT string_agg(DISTINCT kw.name, ', ')
     FROM _ast fn
     JOIN _ast pblock ON pblock.file_path = fn.file_path
                     AND pblock.parent_id = fn.node_id AND pblock.type = 'parameters'
     JOIN _ast tp ON tp.file_path = pblock.file_path
                 AND tp.parent_id = pblock.node_id AND tp.name = p.name
                 AND tp.type IN ('typed_parameter', 'typed_default_parameter')
     JOIN _ast c ON c.file_path = tp.file_path
                AND c.node_id BETWEEN tp.node_id AND tp.node_id + tp.descendant_count
                AND c.type = 'call'
                AND c.name IN ('Header','Cookie','Form','File','Query','Path')
     JOIN _ast kw ON kw.file_path = c.file_path
                 AND kw.node_id BETWEEN c.node_id AND c.node_id + c.descendant_count
                 AND kw.type = 'keyword_argument'
                 AND kw.name IN ('gt','lt','le','ge','min_length','max_length','pattern','regex',
                                 'multiple_of','max_digits','decimal_places','alias')
     WHERE fn.file_path = rr.file_path
       AND fn.parent_id = rr.decorated_id AND fn.type = 'function_definition'
       AND NOT (kw.name IN ('le', 'ge') AND EXISTS (
             SELECT 1 FROM _ast v2
             WHERE v2.file_path = kw.file_path AND v2.parent_id = kw.node_id
               AND v2.type IN ('integer', 'float')))
    ) AS unsupported_constraints,
    -- untyped: param has no type (sitting_duck echoes name as type when untyped)
    (p.type IS NULL OR p.type = p.name) AS is_untyped,
    -- imported model heuristic: PascalCase / dotted type not in local models, not a primitive,
    -- not a DI alias, not a path param, not a FastAPI-injected type, has a type.
    -- Uses effective_type (union-stripped) so `str | None` → `str` → primitive → NOT imported.
    (NOT effective_type IN ('str', 'int', 'float', 'bool', 'bytes', 'list', 'dict', 'None', 'Any')
     AND NOT EXISTS (SELECT 1 FROM _local_models lm WHERE lm.model_name = effective_type)
     AND NOT EXISTS (SELECT 1 FROM _di_aliases da WHERE da.alias_name = effective_type)
     AND NOT EXISTS (SELECT 1 FROM _fastapi_injected fi WHERE fi.alias_name = effective_type)
     AND effective_type != p.name  -- skip un-typed params where sitting_duck echoes the name as type
     AND effective_type IS NOT NULL
     AND NOT array_length(list_filter(
           string_split(rr.full_path, '/'),
           lambda seg: seg = '{' || p.name || '}'
         )) > 0  -- path params are never "imported models" regardless of their type
    ) AS is_imported_model
  FROM _raw_routes rr, UNNEST(rr.parameters) AS t(p)
  -- Exclude DI alias params and FastAPI-injected params (check effective_type and p.type both)
  WHERE NOT EXISTS (SELECT 1 FROM _di_aliases da WHERE da.alias_name = effective_type)
    AND NOT EXISTS (SELECT 1 FROM _fastapi_injected fi WHERE fi.alias_name = effective_type)
)
SELECT
  file_path,
  decorated_id,
  handler_name,
  method,
  full_path,
  start_line,
  param_name,
  param_type,
  -- Path params are ALWAYS required in FastAPI, even when declared with a
  -- `= Path(...)` default (the default is metadata, never a fallback value).
  CASE
    WHEN is_path_param THEN true
    WHEN is_required IS NOT NULL THEN is_required
    ELSE true
  END AS is_required,
  CASE
    WHEN is_path_param                       THEN 'path'
    WHEN explicit_param_fn = 'Header'        THEN 'header'
    WHEN explicit_param_fn = 'Cookie'        THEN 'cookie'
    WHEN explicit_param_fn = 'Form'          THEN 'form'
    WHEN explicit_param_fn = 'File'          THEN 'file'
    WHEN explicit_param_fn = 'Depends'       THEN 'query'  -- inline Depends → treat as query or skip
    WHEN is_local_model                      THEN 'body'
    WHEN is_imported_model                   THEN 'body'
    WHEN is_untyped AND is_path_param        THEN 'path'
    WHEN is_untyped                          THEN 'query'
    ELSE 'query'
  END AS location,
  is_imported_model,
  is_untyped,
  explicit_param_fn,
  constraint_json,
  unsupported_constraints,
  -- qtype uses effective_type so `str | None` maps to 'string', not 'struct'
  CASE effective_type
    WHEN 'str'   THEN 'string'
    WHEN 'int'   THEN 'int'
    WHEN 'float' THEN 'float'
    WHEN 'bool'  THEN 'bool'
    ELSE 'struct'
  END AS qtype
FROM unnested;

-- ── Step 8: class-based view detection ───────────────────────────────────────
-- Fix #6: Only flag when the decorated_definition's ancestor chain contains a
-- class_definition node.  The old heuristic (parent=block) fired on 100
-- false positives in dispatch (model validators, decorators.py, etc.).
-- Now requires: decorated_definition whose parent is 'block' AND that 'block'
-- itself is inside a class_definition (checked via node_id range containment).
CREATE OR REPLACE TEMP TABLE _cbv_routes AS
SELECT dd.file_path, dd.node_id AS decorated_id, dd.start_line
FROM _ast dd
WHERE dd.type = 'decorated_definition'
  AND EXISTS (
    SELECT 1 FROM _ast blk
    WHERE blk.node_id = dd.parent_id AND blk.type = 'block'
      AND blk.file_path = dd.file_path
  )
  AND EXISTS (
    -- Require a class_definition ancestor: its range contains this node
    SELECT 1 FROM _ast cd
    WHERE cd.type = 'class_definition'
      AND cd.file_path = dd.file_path
      AND cd.node_id < dd.node_id
      AND cd.node_id + cd.descendant_count >= dd.node_id
  );

-- ── Step 9: add_api_route detection ──────────────────────────────────────────
CREATE OR REPLACE TEMP TABLE _dynamic_routes AS
SELECT
  c.file_path,
  c.start_line,
  trim(
    (SELECT s.name FROM _ast s
     WHERE s.file_path = c.file_path
       AND s.node_id BETWEEN args.node_id AND args.node_id + args.descendant_count
       AND s.type = 'string'
     ORDER BY s.node_id LIMIT 1),
    '"'''
  ) AS path
FROM _ast c
JOIN _ast args ON args.file_path = c.file_path
              AND args.parent_id = c.node_id AND args.type = 'argument_list'
WHERE c.type = 'call' AND c.name = 'add_api_route';

-- ── Step 10: app.mount detection ─────────────────────────────────────────────
-- Fix #9: Only count .mount( calls whose receiver is a FastAPI/Starlette app var
-- (reuse the router-var guard — any var in _router_var_names).
CREATE OR REPLACE TEMP TABLE _mount_calls AS
SELECT
  c.file_path,
  c.start_line,
  trim(
    (SELECT s.name FROM _ast s
     WHERE s.file_path = c.file_path
       AND s.node_id BETWEEN args.node_id AND args.node_id + args.descendant_count
       AND s.type = 'string'
     ORDER BY s.node_id LIMIT 1),
    '"'''
  ) AS mount_path
FROM _ast c
JOIN _ast args ON args.file_path = c.file_path
              AND args.parent_id = c.node_id AND args.type = 'argument_list'
-- Restrict to calls where the receiver (attribute object) is a known app/router var
WHERE c.type = 'call' AND c.name = 'mount'
  AND EXISTS (
    SELECT 1 FROM _ast attr
    WHERE attr.file_path = c.file_path
      AND attr.parent_id = c.node_id
      AND attr.type = 'attribute'
      -- first identifier in the attribute is the receiver var
      AND EXISTS (
        SELECT 1 FROM _router_var_names rvn
        WHERE rvn.var_name = (
          SELECT id_r.name FROM _ast id_r
          WHERE id_r.file_path = attr.file_path
            AND id_r.parent_id = attr.node_id
            AND id_r.type = 'identifier'
          ORDER BY id_r.node_id ASC LIMIT 1
        )
      )
  );

-- ── Step 11: route migration status ──────────────────────────────────────────
-- A route is NEEDS_REVIEW if it:
--   • is a class-based view, OR
--   • has a non-path param whose type is an imported (unresolvable) model, OR
--   • has a dynamic path (non-literal first arg), OR
--   • has an inline Annotated[T, Param()] param that needs manual verification, OR
--   • has an untyped non-path param.
-- Fix #3: DISTINCT on (file_path, decorated_id, method) prevents multi-decorator
-- duplication (e.g. @router.post + @limiter.limit on same function).
-- Note: path params with complex types (e.g. uuid.UUID, PrimaryKey) do NOT trigger
-- NEEDS_REVIEW — they are cleanly classified as path params.
CREATE OR REPLACE TEMP TABLE _route_status AS
SELECT DISTINCT ON (file_path, decorated_id, method)
  rr.file_path,
  rr.decorated_id,
  rr.handler_name,
  rr.method,
  rr.full_path,
  rr.response_model,
  rr.start_line,
  rr.has_dynamic_path,
  EXISTS (SELECT 1 FROM _cbv_routes cbv WHERE cbv.decorated_id = rr.decorated_id
          AND cbv.file_path = rr.file_path) AS is_cbv,
  EXISTS (
    SELECT 1 FROM _route_params rp
    WHERE rp.decorated_id = rr.decorated_id AND rp.file_path = rr.file_path
      AND rp.is_imported_model
      AND rp.location != 'path'
  ) AS has_imported_model,
  -- Fix #11: Query/Path/Header/Cookie/Form/File params are now cleanly
  -- classified (location + constraint_json) and no longer force review on
  -- their own. Review triggers narrow to: inline Depends (dependency must be
  -- resolved by hand) and unsupported constraint kwargs (dropping them would
  -- validate weaker than the FastAPI original).
  EXISTS (
    SELECT 1 FROM _route_params rp
    WHERE rp.decorated_id = rr.decorated_id AND rp.file_path = rr.file_path
      AND rp.explicit_param_fn = 'Depends'
  ) AS has_inline_depends,
  EXISTS (
    SELECT 1 FROM _route_params rp
    WHERE rp.decorated_id = rr.decorated_id AND rp.file_path = rr.file_path
      AND rp.unsupported_constraints IS NOT NULL
  ) AS has_unsupported_constraint,
  (SELECT string_agg(DISTINCT rp.param_name || ': ' || rp.unsupported_constraints, '; ')
   FROM _route_params rp
   WHERE rp.decorated_id = rr.decorated_id AND rp.file_path = rr.file_path
     AND rp.unsupported_constraints IS NOT NULL
  ) AS unsupported_constraint_detail,
  EXISTS (
    SELECT 1 FROM _route_params rp
    WHERE rp.decorated_id = rr.decorated_id AND rp.file_path = rr.file_path
      AND rp.is_untyped AND rp.location != 'path'
  ) AS has_untyped_query_param,
  CASE
    WHEN EXISTS (SELECT 1 FROM _cbv_routes cbv WHERE cbv.decorated_id = rr.decorated_id
                 AND cbv.file_path = rr.file_path)
      THEN 'NEEDS_REVIEW'
    WHEN rr.has_dynamic_path
      THEN 'NEEDS_REVIEW'
    WHEN EXISTS (
      SELECT 1 FROM _route_params rp
      WHERE rp.decorated_id = rr.decorated_id AND rp.file_path = rr.file_path
        AND rp.is_imported_model AND rp.location != 'path'
    ) THEN 'NEEDS_REVIEW'
    WHEN EXISTS (
      SELECT 1 FROM _route_params rp
      WHERE rp.decorated_id = rr.decorated_id AND rp.file_path = rr.file_path
        AND (rp.explicit_param_fn = 'Depends' OR rp.unsupported_constraints IS NOT NULL)
    ) THEN 'NEEDS_REVIEW'
    ELSE 'MIGRATED'
  END AS status
FROM _raw_routes rr;

-- ── Step 12: emit quackapi registration SQL ───────────────────────────────────
-- Route ID derives from handler_name (snake_case + '_route' suffix).
-- Handler SQL is a TODO placeholder — you supply the DuckDB SQL.
-- param_schema rows use route_id matching the register_route call above.
SELECT
  '-- ============================================================' || chr(10) ||
  '-- quackapi registration — generated by migrate/migrate_fastapi.sql' || chr(10) ||
  '-- Source: ' || getenv('QUACKAPI_SRC') || chr(10) ||
  '-- MIGRATED routes: complete (write handler SQL). NEEDS_REVIEW: see comments.' || chr(10) ||
  '-- Run migrate/COVERAGE.sql in the same session for the full safety report.' || chr(10) ||
  '-- ============================================================' AS registration_sql

UNION ALL

-- Model reference block
SELECT
  chr(10) || '-- ── Model Definitions (BaseModel / SQLModel subclasses) ──────' || chr(10) ||
  string_agg(
    '-- model: ' || lm.model_name || ' (' || lm.file_path || ':' || lm.start_line || ')' || chr(10) ||
    (
      SELECT string_agg(
        '--   ' || mf.field_name || ': ' || mf.field_type ||
        CASE WHEN mf.has_default THEN ' [optional]' ELSE ' [required]' END,
        chr(10)
        ORDER BY mf.field_name
      )
      FROM _model_fields mf WHERE mf.model_name = lm.model_name AND mf.file_path = lm.file_path
    ),
    chr(10)
  ) AS registration_sql
FROM _local_models lm

UNION ALL

-- DI alias reference block
SELECT
  chr(10) || '-- ── Dependency-Injection Aliases (excluded from param_schema) ─' || chr(10) ||
  string_agg('-- DI alias: ' || alias_name, chr(10) ORDER BY alias_name) AS registration_sql
FROM _di_aliases

UNION ALL

-- Route + param_schema registrations
SELECT
  chr(10) || '-- ── Route Registrations ─────────────────────────────────────────' AS registration_sql

UNION ALL

SELECT
  string_agg(
    route_block,
    chr(10) || chr(10)
    ORDER BY file_path, start_line
  ) AS registration_sql
FROM (
  SELECT
    rs.file_path,
    rs.start_line,
    CASE rs.status
      WHEN 'MIGRATED' THEN
        '-- [MIGRATED] ' || rs.handler_name ||
        '  ' || rs.file_path || ':' || rs.start_line ||
        CASE WHEN rs.response_model IS NOT NULL THEN '  response_model=' || rs.response_model ELSE '' END || chr(10) ||
        'INSERT INTO routes SELECT * FROM register_route(' || chr(10) ||
        '  ''' || lower(rs.handler_name) || '_route'',  -- route_id' || chr(10) ||
        '  ''' || rs.method || ''',' || chr(10) ||
        '  ''' || rs.full_path || ''',' || chr(10) ||
        '  ''-- TODO: write DuckDB SQL handler. Params: {' ||
          CASE
            WHEN (SELECT string_agg(rp.param_name, '}, {' ORDER BY rp.param_name)
                  FROM _route_params rp
                  WHERE rp.decorated_id = rs.decorated_id AND rp.file_path = rs.file_path) IS NOT NULL
            THEN (SELECT string_agg(rp.param_name, '}, {' ORDER BY rp.param_name)
                  FROM _route_params rp
                  WHERE rp.decorated_id = rs.decorated_id AND rp.file_path = rs.file_path)
            ELSE 'no params'
          END || '}'',' || chr(10) ||
        '  ''dynamic'',' || chr(10) ||
        '  ''' || rs.handler_name || ''',' || chr(10) ||
        '  200' || chr(10) ||
        ');' ||
        CASE
          WHEN (SELECT string_agg(
                  chr(10) || 'INSERT INTO param_schema VALUES (''' ||
                  lower(rs.handler_name) || '_route'', ''' ||
                  rp.param_name || ''', ''' || rp.location || ''', ''' || rp.qtype || ''', ' ||
                  CASE WHEN rp.is_required THEN 'true' ELSE 'false' END || ', ' || COALESCE('''' || rp.constraint_json || '''', 'NULL') || ');',
                  ''
                  ORDER BY rp.param_name
                )
                FROM _route_params rp
                WHERE rp.decorated_id = rs.decorated_id AND rp.file_path = rs.file_path) IS NOT NULL
          THEN (SELECT string_agg(
                  chr(10) || 'INSERT INTO param_schema VALUES (''' ||
                  lower(rs.handler_name) || '_route'', ''' ||
                  rp.param_name || ''', ''' || rp.location || ''', ''' || rp.qtype || ''', ' ||
                  CASE WHEN rp.is_required THEN 'true' ELSE 'false' END || ', ' || COALESCE('''' || rp.constraint_json || '''', 'NULL') || ');',
                  ''
                  ORDER BY rp.param_name
                )
                FROM _route_params rp
                WHERE rp.decorated_id = rs.decorated_id AND rp.file_path = rs.file_path)
          ELSE ''
        END
      ELSE -- NEEDS_REVIEW
        '-- [NEEDS_REVIEW] ' || rs.handler_name ||
        '  ' || rs.file_path || ':' || rs.start_line ||
        CASE WHEN rs.response_model IS NOT NULL THEN '  response_model=' || rs.response_model ELSE '' END || chr(10) ||
        '-- Reason: ' ||
        CASE
          WHEN rs.is_cbv
            THEN 'class-based view — method is on a class body, not a plain function'
          WHEN rs.has_dynamic_path
            THEN 'dynamic path — first positional arg is not a string literal'
          WHEN rs.has_imported_model
            THEN 'body param type is imported (not in the glob) — field extraction impossible'
          WHEN rs.has_unsupported_constraint
            THEN 'constraint(s) quackapi does not enforce yet [' ||
                 COALESCE(rs.unsupported_constraint_detail, '') ||
                 '] — enforce in handler SQL or extend the runtime, then register'
          WHEN rs.has_inline_depends
            THEN 'inline Annotated[..., Depends(...)] param — resolve the dependency manually'
          ELSE 'unknown'
        END || chr(10) ||
        'INSERT INTO routes SELECT * FROM register_route(' || chr(10) ||
        '  ''' || lower(rs.handler_name) || '_route'',  -- route_id' || chr(10) ||
        '  ''' || rs.method || ''',' || chr(10) ||
        '  ''' || rs.full_path || ''',' || chr(10) ||
        '  ''-- NEEDS_REVIEW: fill handler SQL manually'',' || chr(10) ||
        '  ''dynamic'',' || chr(10) ||
        '  ''' || rs.handler_name || ''',' || chr(10) ||
        '  200' || chr(10) ||
        ');' ||
        CASE
          WHEN (SELECT string_agg(
                  chr(10) ||
                  CASE WHEN rp.is_imported_model AND rp.location != 'path'
                    THEN '-- [NEEDS_REVIEW] param ' || rp.param_name || ': ' || rp.param_type ||
                         ' is an imported type — add its fields to param_schema manually' || chr(10) ||
                         '-- INSERT INTO param_schema VALUES (''' || lower(rs.handler_name) ||
                         '_route'', ''' || rp.param_name || ''', ''body'', ''struct'', ' ||
                         CASE WHEN rp.is_required THEN 'true' ELSE 'false' END || ', ' || COALESCE('''' || rp.constraint_json || '''', 'NULL') || ');'
                    WHEN rp.explicit_param_fn = 'Depends' OR rp.unsupported_constraints IS NOT NULL
                    THEN '-- [NEEDS_REVIEW] param ' || rp.param_name ||
                         CASE WHEN rp.unsupported_constraints IS NOT NULL
                           THEN ': unenforced constraint(s) ' || rp.unsupported_constraints || ' — enforce in handler SQL'
                           ELSE ': Annotated[..., Depends(...)] — resolve the dependency manually'
                         END || chr(10) ||
                         'INSERT INTO param_schema VALUES (''' || lower(rs.handler_name) ||
                         '_route'', ''' || rp.param_name || ''', ''' || rp.location || ''', ''' ||
                         rp.qtype || ''', ' ||
                         CASE WHEN rp.is_required THEN 'true' ELSE 'false' END || ', ' || COALESCE('''' || rp.constraint_json || '''', 'NULL') || ');'
                    ELSE 'INSERT INTO param_schema VALUES (''' || lower(rs.handler_name) ||
                         '_route'', ''' || rp.param_name || ''', ''' || rp.location || ''', ''' ||
                         rp.qtype || ''', ' ||
                         CASE WHEN rp.is_required THEN 'true' ELSE 'false' END || ', ' || COALESCE('''' || rp.constraint_json || '''', 'NULL') || ');'
                  END,
                  ''
                  ORDER BY rp.param_name
                )
                FROM _route_params rp
                WHERE rp.decorated_id = rs.decorated_id AND rp.file_path = rs.file_path) IS NOT NULL
          THEN (SELECT string_agg(
                  chr(10) ||
                  CASE WHEN rp.is_imported_model AND rp.location != 'path'
                    THEN '-- [NEEDS_REVIEW] param ' || rp.param_name || ': ' || rp.param_type ||
                         ' is an imported type — add its fields to param_schema manually' || chr(10) ||
                         '-- INSERT INTO param_schema VALUES (''' || lower(rs.handler_name) ||
                         '_route'', ''' || rp.param_name || ''', ''body'', ''struct'', ' ||
                         CASE WHEN rp.is_required THEN 'true' ELSE 'false' END || ', ' || COALESCE('''' || rp.constraint_json || '''', 'NULL') || ');'
                    WHEN rp.explicit_param_fn = 'Depends' OR rp.unsupported_constraints IS NOT NULL
                    THEN '-- [NEEDS_REVIEW] param ' || rp.param_name ||
                         CASE WHEN rp.unsupported_constraints IS NOT NULL
                           THEN ': unenforced constraint(s) ' || rp.unsupported_constraints || ' — enforce in handler SQL'
                           ELSE ': Annotated[..., Depends(...)] — resolve the dependency manually'
                         END || chr(10) ||
                         'INSERT INTO param_schema VALUES (''' || lower(rs.handler_name) ||
                         '_route'', ''' || rp.param_name || ''', ''' || rp.location || ''', ''' ||
                         rp.qtype || ''', ' ||
                         CASE WHEN rp.is_required THEN 'true' ELSE 'false' END || ', ' || COALESCE('''' || rp.constraint_json || '''', 'NULL') || ');'
                    ELSE 'INSERT INTO param_schema VALUES (''' || lower(rs.handler_name) ||
                         '_route'', ''' || rp.param_name || ''', ''' || rp.location || ''', ''' ||
                         rp.qtype || ''', ' ||
                         CASE WHEN rp.is_required THEN 'true' ELSE 'false' END || ', ' || COALESCE('''' || rp.constraint_json || '''', 'NULL') || ');'
                  END,
                  ''
                  ORDER BY rp.param_name
                )
                FROM _route_params rp
                WHERE rp.decorated_id = rs.decorated_id AND rp.file_path = rs.file_path)
          ELSE ''
        END
    END AS route_block
  FROM _route_status rs
) blocks

UNION ALL

-- 501 catch-all footer
SELECT
  chr(10) ||
  '-- ── 501 Catch-all for un-migrated paths ──────────────────────' || chr(10) ||
  '-- Add this LAST. Paths flagged NEEDS_REVIEW or NOT_DETECTED fall' || chr(10) ||
  '-- through to this and return a clear 501 instead of 404.' || chr(10) ||
  '-- Remove this route once all paths are migrated.' || chr(10) ||
  'INSERT INTO routes SELECT * FROM register_route(' || chr(10) ||
  '  ''not_yet_migrated'',' || chr(10) ||
  '  ''GET'',' || chr(10) ||
  '  ''/unmigrated-catchall'',' || chr(10) ||
  '  ''{\"detail\": \"Route not yet migrated from FastAPI to quackapi.\"}'',' || chr(10) ||
  '  ''static'',' || chr(10) ||
  '  ''Not yet migrated from FastAPI'',' || chr(10) ||
  '  501' || chr(10) ||
  ');' AS registration_sql;
