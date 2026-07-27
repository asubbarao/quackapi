#pragma once
// AUTO-GENERATED from src/sql/*.sql — do not edit by hand.
// Regenerate: python3 scripts/embed_from_x_sql.py
// Source of truth is src/sql/; a sqllogictest asserts byte-equality.

#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace quackapi_from_x_sql {

// src/sql/quack_from_fastapi_routes.sql
static constexpr const char *k_fastapi_routes =
    R"__QUACKAPI_SQL__(-- quack_from_fastapi routes — sitting_duck AST extraction
-- Placeholder: __REPO__  (directory or glob; C++ expands bare dirs to __REPO__/**/*.py)
-- Returns: method, path, handler_name, file, start_line, evidence
WITH source_ast AS (
  SELECT * FROM read_ast(
    '__REPO__',
    'python',
    ignore_errors := true,
    peek := 'full'
  )
),
route_calls AS (
  SELECT
    d.file_path, d.node_id AS decorated_id, dec.start_line,
    c.node_id AS call_id, lower(c.name) AS method_raw, c.peek AS evidence
  FROM source_ast d
  JOIN source_ast dec
    ON dec.file_path = d.file_path AND dec.parent_id = d.node_id AND dec.type = 'decorator'
  JOIN source_ast c
    ON c.file_path = dec.file_path AND c.parent_id = dec.node_id
   AND c.type = 'call' AND c.semantic_type = 'COMPUTATION_CALL'
  WHERE d.type = 'decorated_definition'
    AND lower(c.name) IN (
      'get','post','put','delete','patch','options','head','trace','api_route','websocket'
    )
),
path_args AS (
  SELECT
    rc.*,
    CASE
      WHEN s_raw IS NULL THEN NULL
      WHEN starts_with(s_raw, 'r''') OR starts_with(s_raw, 'r"')
        OR starts_with(s_raw, 'R''') OR starts_with(s_raw, 'R"')
        THEN substring(s_raw, 3, length(s_raw) - 3)
      WHEN length(s_raw) >= 2 AND (
        (s_raw[1] = '''' AND s_raw[length(s_raw)] = '''')
        OR (s_raw[1] = '"' AND s_raw[length(s_raw)] = '"')
      ) THEN substring(s_raw, 2, length(s_raw) - 2)
      ELSE s_raw
    END AS path
  FROM (
    SELECT rc.*, coalesce(s.name, s.peek) AS s_raw
    FROM route_calls rc
    JOIN source_ast al
      ON al.file_path = rc.file_path AND al.parent_id = rc.call_id AND al.type = 'argument_list'
    JOIN source_ast s
      ON s.file_path = rc.file_path AND s.parent_id = al.node_id AND s.type = 'string'
    QUALIFY row_number() OVER (PARTITION BY rc.file_path, rc.call_id ORDER BY s.sibling_index) = 1
  ) rc
)
SELECT
  CASE WHEN method_raw = 'api_route' THEN 'ANY'
       WHEN method_raw = 'websocket' THEN 'WEBSOCKET'
       ELSE upper(method_raw) END AS method,
  p.path,
  fn.name AS handler_name,
  p.file_path AS file,
  p.start_line,
  p.evidence
FROM path_args p
JOIN source_ast fn
  ON fn.file_path = p.file_path AND fn.parent_id = p.decorated_id
 AND fn.type = 'function_definition' AND fn.semantic_type = 'DEFINITION_FUNCTION'
WHERE p.path IS NOT NULL AND p.path <> '' AND fn.name IS NOT NULL
ORDER BY file, start_line, method, path
)__QUACKAPI_SQL__";

// src/sql/quack_from_fastapi_models.sql
static constexpr const char *k_fastapi_models =
    R"__QUACKAPI_SQL__(-- quack_from_fastapi_models — Pydantic BaseModel/SQLModel fields via sitting_duck
-- Placeholder: __REPO__  (directory or glob; C++ expands bare dirs to __REPO__/**/*.py)
-- Returns: model_name, field_name, field_type, is_required, is_optional, has_default,
--          default_expr, file, field_line
WITH source_ast AS (
  SELECT * FROM read_ast(
    '__REPO__',
    'python',
    ignore_errors := true,
    peek := 'full'
  )
),
class_defs AS (
  SELECT node_id, name AS class_name, file_path, start_line, end_line
  FROM source_ast
  WHERE type = 'class_definition' AND semantic_type = 'DEFINITION_CLASS'
    AND name IS NOT NULL AND name <> ''
),
class_bases AS (
  SELECT c.node_id, c.class_name, c.file_path, c.start_line, c.end_line,
         list(DISTINCT CASE
           WHEN b.name IS NULL THEN NULL
           WHEN position('.' IN b.name) > 0 THEN list_last(string_split(b.name, '.'))
           ELSE b.name
         END) FILTER (WHERE b.name IS NOT NULL AND b.name <> '') AS bases
  FROM class_defs c
  JOIN source_ast al
    ON al.file_path = c.file_path AND al.parent_id = c.node_id AND al.type = 'argument_list'
  JOIN source_ast b
    ON b.file_path = c.file_path AND b.parent_id = al.node_id
   AND b.type IN ('identifier', 'attribute')
  GROUP BY ALL
),
pydantic_models AS (
  WITH RECURSIVE reach AS (
    SELECT class_name, file_path, start_line, end_line, node_id, bases, 0 AS depth
    FROM class_bases
    WHERE list_contains(bases, 'BaseModel') OR list_contains(bases, 'SQLModel')
    UNION ALL
    SELECT c.class_name, c.file_path, c.start_line, c.end_line, c.node_id, c.bases, r.depth + 1
    FROM class_bases c
    JOIN reach r ON list_contains(c.bases, r.class_name)
    WHERE r.depth < 16
  )
  SELECT class_name, file_path, start_line, end_line, node_id, bases, min(depth) AS inheritance_depth
  FROM reach GROUP BY ALL
),
field_nodes AS (
  SELECT m.class_name AS model_name, a.name AS field_name,
         a.file_path AS file, a.start_line AS field_line, a.node_id AS field_id
  FROM pydantic_models m
  JOIN source_ast a
    ON a.file_path = m.file_path AND a.scope.class = m.node_id
   AND a.type = 'assignment' AND a.semantic_type = 'DEFINITION_VARIABLE'
   AND a.scope.function IS NULL
  WHERE a.name IS NOT NULL AND a.name <> ''
    AND a.name NOT IN ('model_config', 'Config', 'Meta')
    AND NOT starts_with(a.name, '_')
),
field_types AS (
  SELECT f.field_id, f.file, t.peek AS field_type
  FROM field_nodes f
  JOIN source_ast t
    ON t.file_path = f.file AND t.parent_id = f.field_id AND t.type = 'type'
),
field_defaults AS (
  SELECT f.field_id, f.file, true AS has_default, rhs.peek AS default_expr
  FROM field_nodes f
  JOIN source_ast eq
    ON eq.file_path = f.file AND eq.parent_id = f.field_id AND eq.type = '='
  LEFT JOIN source_ast rhs
    ON rhs.file_path = f.file AND rhs.parent_id = f.field_id
   AND rhs.sibling_index = eq.sibling_index + 1
)
SELECT
  f.model_name,
  f.field_name,
  coalesce(nullif(trim(ft.field_type), ''), 'Any') AS field_type,
  (
    NOT coalesce(fd.has_default, false)
    AND NOT (
      coalesce(ft.field_type, '') ILIKE '%Optional[%'
      OR coalesce(ft.field_type, '') ILIKE '%| None%'
      OR coalesce(ft.field_type, '') ILIKE '%None |%'
    )
  ) AS is_required,
  (
    coalesce(ft.field_type, '') ILIKE '%Optional[%'
    OR coalesce(ft.field_type, '') ILIKE '%| None%'
    OR coalesce(ft.field_type, '') ILIKE '%None |%'
    OR coalesce(fd.default_expr, '') = 'None'
  ) AS is_optional,
  coalesce(fd.has_default, false) AS has_default,
  fd.default_expr,
  f.file,
  f.field_line
FROM field_nodes f
LEFT JOIN field_types ft ON ft.field_id = f.field_id AND ft.file = f.file
LEFT JOIN field_defaults fd ON fd.field_id = f.field_id AND fd.file = f.file
WHERE ft.field_type IS NOT NULL OR fd.has_default
ORDER BY model_name, field_line, field_name
)__QUACKAPI_SQL__";

// src/sql/quack_from_rails_routes.sql
static constexpr const char *k_rails_routes =
    R"__QUACKAPI_SQL__(-- quack_from_rails routes — Rails routes.rb via sitting_duck
-- Placeholder: __REPO__  (app root; C++ maps bare dirs to __REPO__/config/routes.rb)
-- Returns: method, path, handler_name, file, start_line, evidence
WITH
ast AS (
  SELECT * FROM read_ast('__REPO__', 'ruby', peek := 400, ignore_errors := true)
),
dsl AS (
  SELECT node_id, name, start_line, depth, descendant_count, file_path, peek
  FROM ast
  WHERE semantic_type = 'COMPUTATION_CALL'
    AND name IN ('resources','resource','scope','namespace','get','post','put','patch',
                 'delete','root','match','devise_for','member','collection')
),
args AS (
  SELECT d.node_id AS call_id, d.file_path, a.node_id AS arg_id, a.type, a.name, a.peek, a.sibling_index
  FROM dsl d
  JOIN ast al ON al.parent_id = d.node_id AND al.file_path = d.file_path AND al.type = 'argument_list'
  JOIN ast a  ON a.parent_id = al.node_id AND a.file_path = d.file_path
  WHERE a.type NOT IN (',','(',')',':')
),
res AS (
  SELECT call_id, COALESCE(
    (SELECT trim(BOTH ':' FROM name) FROM args x WHERE x.call_id = a.call_id AND x.type = 'simple_symbol'
     ORDER BY sibling_index LIMIT 1),
    (SELECT trim(BOTH '''"' FROM peek) FROM args x WHERE x.call_id = a.call_id AND x.type IN ('string','string_content')
     ORDER BY sibling_index LIMIT 1)
  ) AS rname
  FROM (SELECT DISTINCT call_id FROM args) a
),
pairs AS (
  SELECT a.call_id, a.file_path, hk.name AS k, p.node_id AS pair_id
  FROM args a
  JOIN ast p  ON p.node_id = a.arg_id AND p.file_path = a.file_path AND p.type = 'pair'
  JOIN ast hk ON hk.parent_id = p.node_id AND hk.file_path = a.file_path AND hk.type = 'hash_key_symbol'
),
opt AS (
  SELECT d.node_id,
    COALESCE((SELECT trim(BOTH ':' FROM s.name) FROM pairs op
              JOIN ast s ON s.parent_id = op.pair_id AND s.file_path = op.file_path AND s.type = 'simple_symbol'
              WHERE op.call_id = d.node_id AND op.k = 'param' LIMIT 1), 'id') AS param,
    (SELECT trim(BOTH ':' FROM s.name) FROM pairs op
     JOIN ast s ON s.parent_id = op.pair_id AND s.file_path = op.file_path AND s.type = 'simple_symbol'
     WHERE op.call_id = d.node_id AND op.k = 'on' LIMIT 1) AS on_type,
    (SELECT trim(BOTH '''"' FROM COALESCE(s.name, t.peek)) FROM pairs op
     LEFT JOIN ast s ON s.parent_id = op.pair_id AND s.file_path = op.file_path AND s.type = 'simple_symbol'
     LEFT JOIN ast t ON t.parent_id = op.pair_id AND t.file_path = op.file_path AND t.type = 'string'
     WHERE op.call_id = d.node_id AND op.k = 'to' LIMIT 1) AS to_h,
    (SELECT list(trim(BOTH ':' FROM s.name) ORDER BY s.sibling_index) FROM pairs op
     JOIN ast arr ON arr.parent_id = op.pair_id AND arr.file_path = op.file_path AND arr.type = 'array'
     JOIN ast s ON s.parent_id = arr.node_id AND s.file_path = op.file_path AND s.type = 'simple_symbol'
     WHERE op.call_id = d.node_id AND op.k = 'only') AS only_a,
    (SELECT list(trim(BOTH ':' FROM s.name) ORDER BY s.sibling_index) FROM pairs op
     JOIN ast arr ON arr.parent_id = op.pair_id AND arr.file_path = op.file_path AND arr.type = 'array'
     JOIN ast s ON s.parent_id = arr.node_id AND s.file_path = op.file_path AND s.type = 'simple_symbol'
     WHERE op.call_id = d.node_id AND op.k = 'except') AS except_a
  FROM dsl d
),
reg AS (
  SELECT d.*, r.rname, o.param, o.on_type, o.to_h, o.only_a, o.except_a,
    (SELECT p.node_id FROM dsl p
     WHERE d.file_path = p.file_path AND d.node_id > p.node_id
       AND d.node_id <= p.node_id + p.descendant_count
       AND p.name IN ('resources','resource','scope','namespace','member','collection')
       AND p.node_id <> d.node_id
     ORDER BY p.depth DESC LIMIT 1) AS parent_id
  FROM dsl d
  LEFT JOIN res r ON r.call_id = d.node_id
  LEFT JOIN opt o ON o.node_id = d.node_id
),
path_built AS (
  WITH RECURSIVE pb AS (
    SELECT *, CAST('' AS VARCHAR) AS pref FROM reg WHERE parent_id IS NULL
    UNION ALL
    SELECT c.*, CAST(p.pref || CASE
      WHEN p.name IN ('scope','namespace') AND p.rname IS NOT NULL THEN '/' || p.rname
      WHEN p.name = 'resources' AND p.rname IS NOT NULL THEN '/' || p.rname || '/:' || p.param
      WHEN p.name = 'resource'  AND p.rname IS NOT NULL THEN '/' || p.rname
      ELSE '' END AS VARCHAR)
    FROM reg c JOIN pb p ON c.parent_id = p.node_id AND c.file_path = p.file_path
  ) SELECT * FROM pb
),
rest AS (
  SELECT * FROM (VALUES
    ('index','GET','collection',false),('create','POST','collection',false),
    ('new','GET','collection',true),('show','GET','member',false),
    ('edit','GET','member',true),('update','PUT','member',false),
    ('update','PATCH','member',false),('destroy','DELETE','member',false)
  ) t(action, method, nest, sfx)
),
expanded AS (
  SELECT ra.method,
    CASE
      WHEN pb.name = 'resources' AND ra.nest = 'collection' AND NOT ra.sfx
        THEN pb.pref || '/' || pb.rname
      WHEN pb.name = 'resources' AND ra.nest = 'collection' AND ra.sfx
        THEN pb.pref || '/' || pb.rname || '/' || ra.action
      WHEN pb.name = 'resources' AND ra.nest = 'member' AND NOT ra.sfx
        THEN pb.pref || '/' || pb.rname || '/:' || pb.param
      WHEN pb.name = 'resources' AND ra.nest = 'member' AND ra.sfx
        THEN pb.pref || '/' || pb.rname || '/:' || pb.param || '/' || ra.action
      WHEN pb.name = 'resource' AND ra.action = 'index' THEN NULL
      WHEN pb.name = 'resource' AND ra.sfx THEN pb.pref || '/' || pb.rname || '/' || ra.action
      WHEN pb.name = 'resource' THEN pb.pref || '/' || pb.rname
    END AS path,
    CASE WHEN pb.name = 'resources' THEN pb.rname || '#' || ra.action
         ELSE pb.rname || 's#' || ra.action END AS handler_name,
    pb.file_path AS file, pb.start_line, pb.peek AS evidence
  FROM path_built pb CROSS JOIN rest ra
  WHERE pb.name IN ('resources','resource') AND pb.rname IS NOT NULL
    AND (pb.only_a IS NULL OR list_contains(pb.only_a, ra.action))
    AND (pb.except_a IS NULL OR NOT list_contains(pb.except_a, ra.action))
    AND NOT (pb.name = 'resource' AND ra.action = 'index')

  UNION ALL
  SELECT upper(pb.name),
    CASE
      WHEN pb.on_type = 'collection'
        THEN regexp_replace(pb.pref, '/:[^/]+$', '') || '/' || pb.rname
      WHEN pb.on_type = 'member' THEN pb.pref || '/' || pb.rname
      WHEN pb.rname LIKE '%/%' OR pb.rname LIKE '%:%'
        THEN CASE WHEN pb.rname LIKE '/%' THEN pb.rname ELSE pb.pref || '/' || pb.rname END
      ELSE pb.pref || '/' || pb.rname
    END,
    COALESCE(pb.to_h,
      (SELECT CASE WHEN p.name = 'resource' THEN p.rname || 's' ELSE p.rname END
       FROM path_built p WHERE p.node_id = pb.parent_id) || '#' || pb.rname),
    pb.file_path, pb.start_line, pb.peek
  FROM path_built pb
  WHERE pb.name IN ('get','post','put','patch','delete','match') AND pb.rname IS NOT NULL

  UNION ALL
  SELECT t.m, pb.pref || t.p, 'sessions#' || t.a, pb.file_path, pb.start_line, pb.peek
  FROM path_built pb
  CROSS JOIN (VALUES ('GET','/users/login','new'),('POST','/users/login','create'),
                     ('DELETE','/users/sign_out','destroy')) t(m,p,a)
  WHERE pb.name = 'devise_for'
)
SELECT method, path, handler_name, file, start_line, evidence
FROM expanded WHERE path IS NOT NULL
ORDER BY file, start_line, method, path
)__QUACKAPI_SQL__";

// src/sql/quack_from_rails_models.sql
static constexpr const char *k_rails_models =
    R"__QUACKAPI_SQL__(-- quack_from_rails_models — validates + strong params via sitting_duck
-- Placeholder: __REPO__  (app root; C++ maps bare dirs to models+controllers globs)
-- Returns: model_name, field_name, field_type, is_required, is_optional, has_default,
--          default_expr, file, field_line
WITH
ast AS (
  SELECT * FROM read_ast(
    ['__REPO__/app/models/**/*.rb',
     '__REPO__/app/controllers/**/*.rb'],
    'ruby', peek := 400, ignore_errors := true)
),
classes AS (
  SELECT file_path, node_id, name FROM ast
  WHERE semantic_type = 'DEFINITION_CLASS' AND name IS NOT NULL AND name <> ''
),
val_calls AS (
  SELECT * FROM ast WHERE semantic_type = 'COMPUTATION_CALL' AND name = 'validates'
),
val_fields AS (
  SELECT v.file_path, v.node_id AS cid, v.start_line, v.peek, v.scope.class AS cls,
         trim(BOTH ':' FROM a.name) AS field_name
  FROM val_calls v
  JOIN ast al ON al.parent_id = v.node_id AND al.file_path = v.file_path AND al.type = 'argument_list'
  JOIN ast a  ON a.parent_id = al.node_id AND a.file_path = v.file_path AND a.type = 'simple_symbol'
),
val_opts AS (
  SELECT v.file_path, v.node_id AS cid, hk.name AS vkey
  FROM val_calls v
  JOIN ast al ON al.parent_id = v.node_id AND al.file_path = v.file_path AND al.type = 'argument_list'
  JOIN ast p  ON p.parent_id = al.node_id AND p.file_path = v.file_path AND p.type = 'pair'
  JOIN ast hk ON hk.parent_id = p.node_id AND hk.file_path = v.file_path AND hk.type = 'hash_key_symbol'
),
from_val AS (
  SELECT c.name AS model_name, f.field_name,
         array_to_string(list_sort(list(DISTINCT o.vkey)), ',') AS field_type,
         list_contains(list(DISTINCT o.vkey), 'presence') AS is_required,
         false AS has_default, CAST(NULL AS VARCHAR) AS default_expr,
         f.file_path AS file, f.start_line AS field_line
  FROM val_fields f
  JOIN classes c ON c.node_id = f.cls AND c.file_path = f.file_path
  LEFT JOIN val_opts o ON o.cid = f.cid AND o.file_path = f.file_path
  GROUP BY ALL
),
permits AS (
  SELECT p.* FROM ast p
  WHERE p.semantic_type = 'COMPUTATION_CALL' AND p.name = 'permit'
    AND EXISTS (SELECT 1 FROM ast r WHERE r.parent_id = p.node_id AND r.file_path = p.file_path
                  AND r.semantic_type = 'COMPUTATION_CALL' AND r.name = 'require')
),
req_model AS (
  SELECT p.node_id AS pid, p.file_path, p.start_line, p.peek, p.scope.class AS cls,
         trim(BOTH ':' FROM s.name) AS model_key
  FROM permits p
  JOIN ast r  ON r.parent_id = p.node_id AND r.file_path = p.file_path
             AND r.semantic_type = 'COMPUTATION_CALL' AND r.name = 'require'
  JOIN ast ra ON ra.parent_id = r.node_id AND ra.file_path = p.file_path AND ra.type = 'argument_list'
  JOIN ast s  ON s.parent_id = ra.node_id AND s.file_path = p.file_path AND s.type = 'simple_symbol'
),
pfields AS (
  SELECT p.node_id AS pid, p.file_path, trim(BOTH ':' FROM s.name) AS field_name, 'strong_param' AS field_type
  FROM permits p
  JOIN ast pa ON pa.parent_id = p.node_id AND pa.file_path = p.file_path AND pa.type = 'argument_list'
  JOIN ast s  ON s.parent_id = pa.node_id AND s.file_path = p.file_path AND s.type = 'simple_symbol'
  UNION ALL
  SELECT p.node_id, p.file_path, hk.name, 'array'
  FROM permits p
  JOIN ast pa ON pa.parent_id = p.node_id AND pa.file_path = p.file_path AND pa.type = 'argument_list'
  JOIN ast pr ON pr.parent_id = pa.node_id AND pr.file_path = p.file_path AND pr.type = 'pair'
  JOIN ast hk ON hk.parent_id = pr.node_id AND hk.file_path = p.file_path AND hk.type = 'hash_key_symbol'
),
from_sp AS (
  SELECT upper(substr(rm.model_key,1,1)) || substr(rm.model_key,2) AS model_name,
         pf.field_name, pf.field_type, true AS is_required,
         false AS has_default, CAST(NULL AS VARCHAR) AS default_expr,
         rm.file_path AS file, rm.start_line AS field_line
  FROM req_model rm
  JOIN pfields pf ON pf.pid = rm.pid AND pf.file_path = rm.file_path
),
models AS (
  SELECT model_name, field_name, field_type, is_required,
         NOT is_required AS is_optional, has_default, default_expr, file, field_line
  FROM from_val
  UNION ALL
  SELECT model_name, field_name, field_type, is_required,
         NOT is_required AS is_optional, has_default, default_expr, file, field_line
  FROM from_sp
)
SELECT model_name, field_name, field_type, is_required, is_optional, has_default,
       default_expr, file, field_line
FROM models
ORDER BY model_name, field_name
)__QUACKAPI_SQL__";

// src/sql/quack_from_express_routes.sql
static constexpr const char *k_express_routes =
    R"__QUACKAPI_SQL__(-- quack_from_express routes — Express HTTP routes via sitting_duck
-- Placeholder: __REPO__  (app root; C++ expands bare dirs to ts+js globs)
-- Returns: method, path, handler_name, file, start_line, evidence
WITH express_ast AS (
  SELECT * FROM read_ast('__REPO__/**/*.ts', 'typescript', peek := 400, ignore_errors := true)
  UNION ALL BY NAME
  SELECT * FROM read_ast('__REPO__/**/*.js', 'javascript', peek := 400, ignore_errors := true)
),
route_calls AS (
  SELECT c.*,
    (SELECT trim(s.name, '''"')
     FROM express_ast a JOIN express_ast s ON s.parent_id = a.node_id AND s.file_path = a.file_path
     WHERE a.parent_id = c.node_id AND a.file_path = c.file_path AND a.type = 'arguments' AND s.type = 'string'
     ORDER BY s.sibling_index LIMIT 1) AS path_raw,
    (SELECT obj.name FROM express_ast m JOIN express_ast obj
       ON obj.parent_id = m.node_id AND obj.file_path = m.file_path AND obj.type = 'identifier'
     WHERE m.parent_id = c.node_id AND m.file_path = c.file_path AND m.type = 'member_expression'
     ORDER BY obj.sibling_index LIMIT 1) AS router_obj
  FROM express_ast c
  WHERE c.type = 'call_expression'
    AND lower(c.name) IN ('get','post','put','delete','patch','all','options','head')
    AND EXISTS (SELECT 1 FROM express_ast m
                WHERE m.parent_id = c.node_id AND m.file_path = c.file_path AND m.type = 'member_expression')
),
routes_base AS (
  SELECT * FROM route_calls
  WHERE path_raw IS NOT NULL AND (path_raw LIKE '/%' OR path_raw IN ('*') OR path_raw LIKE '*%')
),
handlers AS (
  SELECT rb.node_id AS rid, rb.file_path, h.node_id AS hid, h.type AS htype,
         h.name AS hname, h.start_line AS hs, h.end_line AS he, h.depth AS hdepth
  FROM routes_base rb
  JOIN express_ast a ON a.parent_id = rb.node_id AND a.file_path = rb.file_path AND a.type = 'arguments'
  JOIN express_ast h ON h.parent_id = a.node_id AND h.file_path = a.file_path
    AND h.type IN ('arrow_function','function_expression','function_declaration',
                   'identifier','member_expression','call_expression')
  QUALIFY row_number() OVER (PARTITION BY rb.node_id, rb.file_path ORDER BY h.sibling_index DESC) = 1
),
handler_names AS (
  SELECT h.rid, h.file_path,
    CASE
      WHEN h.htype = 'identifier' THEN nullif(h.hname, '')
      WHEN h.htype = 'member_expression' THEN COALESCE(
        (SELECT p.name FROM express_ast p WHERE p.parent_id = h.hid AND p.file_path = h.file_path
           AND p.type = 'property_identifier' ORDER BY p.sibling_index DESC LIMIT 1),
        nullif(h.hname, ''), 'member_handler')
      WHEN h.htype = 'call_expression' THEN COALESCE(nullif(h.hname, ''), 'bound_handler')
      ELSE COALESCE(nullif(h.hname, ''),
        (SELECT c.name FROM express_ast c
         WHERE c.file_path = h.file_path AND c.type = 'call_expression'
           AND c.semantic_type = 'COMPUTATION_CALL'
           AND c.start_line BETWEEN h.hs AND h.he AND c.end_line <= h.he
           AND c.depth > h.hdepth AND nullif(c.name,'') IS NOT NULL
           AND lower(c.name) NOT IN (
             'json','status','send','sendstatus','next','jsonp','redirect','render','end',
             'set','cookie','clearcookie','get','post','put','delete','patch','all','use')
         ORDER BY c.start_line, c.node_id LIMIT 1),
        'anonymous')
    END AS handler_name
  FROM handlers h
),
string_mounts AS (
  SELECT c.file_path, trim(s.name, '''"') AS prefix
  FROM express_ast c
  JOIN express_ast m ON m.parent_id = c.node_id AND m.file_path = c.file_path AND m.type = 'member_expression'
  JOIN express_ast a ON a.parent_id = c.node_id AND a.file_path = c.file_path AND a.type = 'arguments'
  JOIN express_ast s ON s.parent_id = a.node_id AND s.file_path = a.file_path AND s.type = 'string'
  WHERE c.type = 'call_expression' AND lower(c.name) = 'use' AND trim(s.name, '''"') LIKE '/%'
  QUALIFY row_number() OVER (PARTITION BY c.node_id, c.file_path ORDER BY s.sibling_index) = 1
),
bare_mounts AS (
  SELECT c.file_path, id.name AS mounted_ident
  FROM express_ast c
  JOIN express_ast m ON m.parent_id = c.node_id AND m.file_path = c.file_path AND m.type = 'member_expression'
  JOIN express_ast a ON a.parent_id = c.node_id AND a.file_path = c.file_path AND a.type = 'arguments'
  JOIN express_ast id ON id.parent_id = a.node_id AND id.file_path = a.file_path AND id.type = 'identifier'
  WHERE c.type = 'call_expression' AND lower(c.name) = 'use'
    AND NOT EXISTS (SELECT 1 FROM express_ast s WHERE s.parent_id = a.node_id AND s.file_path = a.file_path
                      AND s.type = 'string' AND s.sibling_index < id.sibling_index)
),
imports AS (
  SELECT imp.file_path AS from_file,
    COALESCE(
      (SELECT id.name FROM express_ast clause
       JOIN express_ast id ON id.parent_id = clause.node_id AND id.file_path = clause.file_path
       WHERE clause.parent_id = imp.node_id AND clause.file_path = imp.file_path
         AND clause.type = 'import_clause' AND id.type = 'identifier' AND id.name IS NOT NULL
       ORDER BY id.sibling_index LIMIT 1),
      (SELECT sp.name FROM express_ast n
       JOIN express_ast sp ON sp.parent_id = n.node_id AND sp.file_path = n.file_path
       WHERE n.parent_id IN (
           SELECT c.node_id FROM express_ast c
           WHERE c.parent_id = imp.node_id AND c.file_path = imp.file_path AND c.type = 'import_clause')
         AND n.file_path = imp.file_path AND n.type = 'named_imports'
         AND sp.type = 'import_specifier' AND sp.name IS NOT NULL
       ORDER BY sp.sibling_index LIMIT 1)
    ) AS local_name,
    COALESCE(
      trim(imp.name, '''"'),
      trim((SELECT s.name FROM express_ast s WHERE s.parent_id = imp.node_id AND s.file_path = imp.file_path
              AND s.type = 'string' ORDER BY s.sibling_index DESC LIMIT 1), '''"')
    ) AS source_mod
  FROM express_ast imp
  WHERE imp.type = 'import_statement' AND imp.semantic_type = 'EXTERNAL_IMPORT'
),
file_prefixes AS (
  SELECT DISTINCT sm.prefix, replace(replace(im.source_mod, './', ''), '../', '') AS source_key
  FROM string_mounts sm
  JOIN bare_mounts bm ON bm.file_path = sm.file_path
  JOIN imports im ON im.from_file = bm.file_path AND im.local_name = bm.mounted_ident
)
SELECT lower(rb.name) AS method,
  CASE
    WHEN fp.prefix IS NOT NULL AND rb.path_raw = '/' THEN fp.prefix
    WHEN fp.prefix IS NOT NULL AND starts_with(rb.path_raw, fp.prefix) THEN rb.path_raw
    WHEN fp.prefix IS NOT NULL THEN rtrim(fp.prefix, '/') || '/' || ltrim(rb.path_raw, '/')
    ELSE rb.path_raw
  END AS path,
  COALESCE(hn.handler_name, 'anonymous') AS handler_name,
  rb.file_path AS file, rb.start_line,
  left(rb.peek, 200) AS evidence
FROM routes_base rb
LEFT JOIN handler_names hn ON hn.rid = rb.node_id AND hn.file_path = rb.file_path
LEFT JOIN file_prefixes fp
  ON rb.file_path LIKE '%' || fp.source_key || '%'
  OR rb.file_path LIKE '%' || fp.source_key || '.ts'
  OR rb.file_path LIKE '%' || fp.source_key || '.js'
ORDER BY file, start_line, method, path
)__QUACKAPI_SQL__";

// src/sql/quack_from_express_models.sql
static constexpr const char *k_express_models =
    R"__QUACKAPI_SQL__(-- quack_from_express_models — TS interfaces/classes/zod via sitting_duck
-- Placeholder: __REPO__  (app root; C++ expands bare dirs to ts+js globs)
-- Returns: model_name, field_name, field_type, is_required, is_optional, has_default,
--          default_expr, file, field_line
WITH express_ast AS (
  SELECT * FROM read_ast('__REPO__/**/*.ts', 'typescript', peek := 400, ignore_errors := true)
  UNION ALL BY NAME
  SELECT * FROM read_ast('__REPO__/**/*.js', 'javascript', peek := 400, ignore_errors := true)
),
interface_fields AS (
  SELECT cls.name AS model_name, ps.name AS field_name,
    COALESCE(
      (SELECT nullif(trim(trim(ta.peek), ': '), '') FROM express_ast ta
       WHERE ta.parent_id = ps.node_id AND ta.file_path = ps.file_path AND ta.type = 'type_annotation' LIMIT 1),
      (SELECT pi.signature_type FROM express_ast pi
       WHERE pi.parent_id = ps.node_id AND pi.file_path = ps.file_path AND pi.type = 'property_identifier' LIMIT 1),
      'unknown') AS field_type,
    EXISTS (SELECT 1 FROM express_ast q WHERE q.parent_id = ps.node_id AND q.file_path = ps.file_path AND q.type = '?') AS is_optional,
    false AS has_default, CAST(NULL AS VARCHAR) AS default_expr,
    ps.file_path AS file, ps.start_line AS field_line
  FROM express_ast ps
  JOIN express_ast cls ON cls.node_id = ps.scope.class AND cls.file_path = ps.file_path
    AND cls.type IN ('interface_declaration','class_declaration','type_alias_declaration')
  WHERE ps.type = 'property_signature' AND ps.name IS NOT NULL AND cls.name IS NOT NULL
),
class_fields AS (
  SELECT cls.name AS model_name, f.name AS field_name,
    COALESCE(
      (SELECT nullif(trim(trim(ta.peek), ': '), '') FROM express_ast ta
       WHERE ta.parent_id = f.node_id AND ta.file_path = f.file_path AND ta.type = 'type_annotation' LIMIT 1),
      f.signature_type, 'unknown') AS field_type,
    (EXISTS (SELECT 1 FROM express_ast q WHERE q.parent_id = f.node_id AND q.file_path = f.file_path AND q.type = '?')
      OR coalesce(f.annotations,'') ILIKE '%IsOptional%'
      OR coalesce(f.peek,'') LIKE '%?:%') AS is_optional,
    (EXISTS (SELECT 1 FROM express_ast eq WHERE eq.parent_id = f.node_id AND eq.file_path = f.file_path
               AND eq.type IN ('=','initializer','assignment_expression'))
      OR regexp_matches(coalesce(f.peek,''), '=\s*\S')) AS has_default,
    CASE WHEN position('=' IN coalesce(f.peek,'')) > 0 THEN trim(regexp_extract(f.peek, '=\s*(.+)$', 1)) END AS default_expr,
    f.file_path AS file, f.start_line AS field_line
  FROM express_ast f
  JOIN express_ast cls ON cls.node_id = f.scope.class AND cls.file_path = f.file_path AND cls.type = 'class_declaration'
  WHERE f.type IN ('public_field_definition','field_definition','property_definition')
    AND f.name IS NOT NULL AND cls.name IS NOT NULL
),
ctor_fields AS (
  SELECT cls.name AS model_name, p.name AS field_name,
    COALESCE(
      (SELECT nullif(trim(trim(ta.peek), ': '), '') FROM express_ast ta
       WHERE ta.parent_id = p.node_id AND ta.file_path = p.file_path AND ta.type = 'type_annotation' LIMIT 1),
      'unknown') AS field_type,
    (p.type = 'optional_parameter') AS is_optional, false AS has_default, CAST(NULL AS VARCHAR) AS default_expr,
    p.file_path AS file, p.start_line AS field_line
  FROM express_ast p
  JOIN express_ast cls ON cls.node_id = p.scope.class AND cls.file_path = p.file_path AND cls.type = 'class_declaration'
  WHERE p.type IN ('required_parameter','optional_parameter') AND p.name IS NOT NULL
    AND (list_contains(coalesce(p.modifiers, []), 'public')
      OR list_contains(coalesce(p.modifiers, []), 'private')
      OR list_contains(coalesce(p.modifiers, []), 'protected')
      OR list_contains(coalesce(p.modifiers, []), 'readonly')
      OR p.peek ILIKE 'public %' OR p.peek ILIKE 'private %'
      OR p.peek ILIKE 'protected %' OR p.peek ILIKE 'readonly %')
),
zod_objects AS (
  SELECT c.node_id, c.file_path,
    COALESCE(
      (SELECT v.name FROM express_ast v WHERE v.file_path = c.file_path AND v.type = 'variable_declarator'
         AND v.name IS NOT NULL AND c.start_line BETWEEN v.start_line AND v.end_line AND v.depth < c.depth
       ORDER BY v.depth DESC LIMIT 1), 'ZodObject') AS model_name
  FROM express_ast c
  WHERE c.type = 'call_expression' AND c.name = 'object'
    AND EXISTS (
      SELECT 1 FROM express_ast m WHERE m.parent_id = c.node_id AND m.file_path = c.file_path
        AND m.type = 'member_expression'
        AND (m.peek LIKE 'z.object%' OR m.peek LIKE 'zod.object%'
          OR EXISTS (SELECT 1 FROM express_ast id WHERE id.parent_id = m.node_id AND id.file_path = m.file_path
                       AND id.type = 'identifier' AND lower(id.name) IN ('z','zod'))))
),
zod_fields AS (
  SELECT zo.model_name, COALESCE(key.name, key.peek) AS field_name,
    COALESCE((SELECT left(val.peek, 80) FROM express_ast val
              WHERE val.parent_id = pair.node_id AND val.file_path = pair.file_path
                AND val.node_id <> key.node_id AND val.type NOT IN (':',',','}')
              ORDER BY val.sibling_index DESC LIMIT 1), 'zod') AS field_type,
    COALESCE((SELECT left(val.peek, 80) FROM express_ast val
              WHERE val.parent_id = pair.node_id AND val.file_path = pair.file_path
                AND val.node_id <> key.node_id AND val.type NOT IN (':',',','}')
              ORDER BY val.sibling_index DESC LIMIT 1), '') ILIKE '%optional%' AS is_optional,
    COALESCE((SELECT left(val.peek, 80) FROM express_ast val
              WHERE val.parent_id = pair.node_id AND val.file_path = pair.file_path
                AND val.node_id <> key.node_id AND val.type NOT IN (':',',','}')
              ORDER BY val.sibling_index DESC LIMIT 1), '') ILIKE '%default%' AS has_default,
    CAST(NULL AS VARCHAR) AS default_expr,
    zo.file_path AS file, pair.start_line AS field_line
  FROM zod_objects zo
  JOIN express_ast a ON a.parent_id = zo.node_id AND a.file_path = zo.file_path AND a.type = 'arguments'
  JOIN express_ast obj ON obj.parent_id = a.node_id AND obj.file_path = a.file_path AND obj.type IN ('object','object_expression')
  JOIN express_ast pair ON pair.parent_id = obj.node_id AND pair.file_path = obj.file_path AND pair.type = 'pair'
  JOIN express_ast key ON key.parent_id = pair.node_id AND key.file_path = pair.file_path
    AND key.type IN ('property_identifier','identifier','string')
  QUALIFY row_number() OVER (PARTITION BY pair.node_id, pair.file_path ORDER BY key.sibling_index) = 1
),
models AS (
  SELECT model_name, field_name, field_type, (NOT is_optional) AS is_required, is_optional,
         has_default, default_expr, file, field_line FROM interface_fields
  UNION ALL BY NAME
  SELECT model_name, field_name, field_type, (NOT is_optional) AS is_required, is_optional,
         has_default, default_expr, file, field_line FROM class_fields
  UNION ALL BY NAME
  SELECT model_name, field_name, field_type, (NOT is_optional) AS is_required, is_optional,
         has_default, default_expr, file, field_line FROM ctor_fields
  UNION ALL BY NAME
  SELECT model_name, field_name, field_type, (NOT coalesce(is_optional,false)) AS is_required,
         coalesce(is_optional,false) AS is_optional, coalesce(has_default,false) AS has_default,
         default_expr, file, field_line FROM zod_fields
)
SELECT model_name, field_name, field_type, is_required, is_optional, has_default,
       default_expr, file, field_line
FROM models
ORDER BY model_name, field_line, field_name
)__QUACKAPI_SQL__";

// src/sql/quack_from_gin_routes.sql
static constexpr const char *k_gin_routes =
    R"__QUACKAPI_SQL__(-- quack_from_gin routes — Gin HTTP routes via sitting_duck
-- Placeholder: __REPO__  (app root; C++ expands bare dirs to __REPO__/**/*.go)
-- Returns: method, path, handler_name, file, start_line, evidence
WITH
ast AS (
  SELECT * FROM read_ast('__REPO__', 'go', ignore_errors := true, peek := 300)
),
route_calls AS (
  SELECT c.file_path, c.node_id AS route_id, c.name AS method_raw, c.start_line,
         c.peek AS evidence, c.scope.function AS func_scope_id
  FROM ast c
  WHERE c.semantic_type = 'COMPUTATION_CALL'
    AND c.name IN ('GET','POST','PUT','DELETE','PATCH','HEAD','OPTIONS','Any','Handle')
),
receivers AS (
  SELECT r.file_path, r.route_id, id.name AS receiver
  FROM route_calls r
  JOIN ast sel ON sel.file_path = r.file_path AND sel.parent_id = r.route_id
              AND sel.type = 'selector_expression'
  JOIN ast id  ON id.file_path = sel.file_path AND id.parent_id = sel.node_id
              AND id.type = 'identifier'
),
args AS (
  SELECT r.file_path, r.route_id, a.sibling_index, a.type AS arg_type,
         a.name AS arg_name, a.peek AS arg_peek
  FROM route_calls r
  JOIN ast al ON al.file_path = r.file_path AND al.parent_id = r.route_id
             AND al.type = 'argument_list'
  JOIN ast a  ON a.file_path = al.file_path AND a.parent_id = al.node_id
             AND a.type IN ('interpreted_string_literal','identifier',
                            'selector_expression','func_literal')
),
route_core AS (
  SELECT r.*, rv.receiver,
    CASE WHEN r.method_raw = 'Handle' THEN (
      SELECT trim(a.arg_peek,'"') FROM args a
      WHERE a.file_path=r.file_path AND a.route_id=r.route_id
        AND a.arg_type='interpreted_string_literal'
      ORDER BY a.sibling_index OFFSET 1 LIMIT 1)
    ELSE (
      SELECT trim(a.arg_peek,'"') FROM args a
      WHERE a.file_path=r.file_path AND a.route_id=r.route_id
        AND a.arg_type='interpreted_string_literal'
      ORDER BY a.sibling_index LIMIT 1)
    END AS rel_path,
    CASE WHEN r.method_raw='Handle' THEN (
      SELECT upper(trim(a.arg_peek,'"')) FROM args a
      WHERE a.file_path=r.file_path AND a.route_id=r.route_id
        AND a.arg_type='interpreted_string_literal'
      ORDER BY a.sibling_index LIMIT 1)
    WHEN r.method_raw='Any' THEN 'ANY' ELSE r.method_raw END AS method,
    COALESCE(
      (SELECT COALESCE(a.arg_name, regexp_extract(a.arg_peek,'([A-Za-z_][A-Za-z0-9_]*)$',1))
       FROM args a WHERE a.file_path=r.file_path AND a.route_id=r.route_id
         AND a.arg_type IN ('identifier','selector_expression')
       ORDER BY a.sibling_index DESC LIMIT 1),
      (SELECT '<inline>' FROM args a
       WHERE a.file_path=r.file_path AND a.route_id=r.route_id
         AND a.arg_type='func_literal' LIMIT 1)
    ) AS handler_name
  FROM route_calls r
  LEFT JOIN receivers rv ON rv.file_path=r.file_path AND rv.route_id=r.route_id
),
gin_routes AS (
  SELECT rc.*, f.name AS enclosing_fn
  FROM route_core rc
  LEFT JOIN ast f ON f.file_path=rc.file_path AND f.node_id=rc.func_scope_id
                 AND f.semantic_type='DEFINITION_FUNCTION'
  WHERE rc.rel_path IS NOT NULL
    AND (rc.rel_path='' OR rc.rel_path='/' OR rc.rel_path LIKE '/%')
),
gin_groups AS (
  SELECT g.file_path, g.node_id AS group_id,
    (SELECT id.name FROM ast sel
     JOIN ast id ON id.file_path=sel.file_path AND id.parent_id=sel.node_id
                AND id.type='identifier'
     WHERE sel.file_path=g.file_path AND sel.parent_id=g.node_id
       AND sel.type='selector_expression' LIMIT 1) AS receiver,
    (SELECT trim(s.peek,'"') FROM ast al
     JOIN ast s ON s.file_path=al.file_path AND s.parent_id=al.node_id
               AND s.type='interpreted_string_literal'
     WHERE al.file_path=g.file_path AND al.parent_id=g.node_id
       AND al.type='argument_list' ORDER BY s.sibling_index LIMIT 1) AS path_seg
  FROM ast g
  WHERE g.semantic_type='COMPUTATION_CALL' AND g.name='Group'
),
gin_groups_f AS (
  SELECT * FROM gin_groups
  WHERE path_seg IS NOT NULL AND (path_seg='' OR path_seg='/' OR path_seg LIKE '/%')
),
group_bindings AS (
  SELECT DISTINCT gg.file_path, gg.group_id, gg.receiver, gg.path_seg,
                  left_id.name AS bound_var
  FROM gin_groups_f gg
  JOIN ast svd ON svd.file_path=gg.file_path AND svd.type='short_var_declaration'
   AND gg.group_id IN (
     SELECT d.node_id FROM ast d
     WHERE d.file_path=svd.file_path
       AND d.start_line>=svd.start_line AND d.end_line<=svd.end_line
       AND d.type='call_expression' AND d.name='Group')
  JOIN ast left_list ON left_list.file_path=svd.file_path
    AND left_list.parent_id=svd.node_id AND left_list.type='expression_list'
    AND left_list.sibling_index=0
  JOIN ast left_id ON left_id.file_path=left_list.file_path
    AND left_id.parent_id=left_list.node_id AND left_id.type='identifier'
),
group_prefix_resolved AS (
  WITH RECURSIVE chain AS (
    SELECT b.file_path, b.bound_var, b.path_seg AS full_prefix, b.receiver, 0 AS depth
    FROM group_bindings b
    WHERE NOT EXISTS (SELECT 1 FROM group_bindings p
                      WHERE p.file_path=b.file_path AND p.bound_var=b.receiver)
    UNION ALL
    SELECT c.file_path, b.bound_var,
      CASE WHEN c.full_prefix='' THEN b.path_seg
           WHEN b.path_seg IN ('','/') THEN rtrim(c.full_prefix,'/')||b.path_seg
           ELSE rtrim(c.full_prefix,'/')||
             CASE WHEN starts_with(b.path_seg,'/') THEN b.path_seg ELSE '/'||b.path_seg END
      END, b.receiver, c.depth+1
    FROM chain c
    JOIN group_bindings b ON b.file_path=c.file_path AND b.receiver=c.bound_var
    WHERE c.depth < 8
  )
  SELECT file_path, bound_var, full_prefix FROM chain
),
register_prefix_best AS (
  SELECT register_fn, full_prefix FROM (
    SELECT c.name AS register_fn,
      CASE WHEN gp.full_prefix IS NULL OR gp.full_prefix='' THEN gg.path_seg
           WHEN gg.path_seg IN ('','/') THEN rtrim(gp.full_prefix,'/')||gg.path_seg
           ELSE rtrim(gp.full_prefix,'/')||
             CASE WHEN starts_with(gg.path_seg,'/') THEN gg.path_seg ELSE '/'||gg.path_seg END
      END AS full_prefix,
      row_number() OVER (PARTITION BY c.name ORDER BY
        (c.file_path NOT LIKE '%_test.go') DESC,
        length(COALESCE(gp.full_prefix,'')||gg.path_seg) DESC) AS rn
    FROM ast c
    JOIN ast al ON al.file_path=c.file_path AND al.parent_id=c.node_id
               AND al.type='argument_list'
    JOIN gin_groups_f gg ON gg.file_path=c.file_path AND gg.group_id IN (
      SELECT a.node_id FROM ast a WHERE a.file_path=al.file_path
        AND a.parent_id=al.node_id AND a.type='call_expression' AND a.name='Group')
    LEFT JOIN group_prefix_resolved gp
      ON gp.file_path=gg.file_path AND gp.bound_var=gg.receiver
    WHERE c.semantic_type='COMPUTATION_CALL'
      AND c.name NOT IN ('Group','GET','POST','PUT','DELETE','PATCH',
                         'HEAD','OPTIONS','Any','Handle','Use')
  ) x WHERE rn=1
)
SELECT gr.method,
  CASE WHEN pref.prefix IS NULL OR pref.prefix='' THEN
         CASE WHEN gr.rel_path='' THEN '/' ELSE gr.rel_path END
       WHEN gr.rel_path IS NULL OR gr.rel_path='' THEN pref.prefix
       WHEN gr.rel_path='/' THEN rtrim(pref.prefix,'/')||'/'
       ELSE rtrim(pref.prefix,'/')||
         CASE WHEN starts_with(gr.rel_path,'/') THEN gr.rel_path ELSE '/'||gr.rel_path END
  END AS path,
  gr.handler_name, gr.file_path AS file, gr.start_line, gr.evidence
FROM gin_routes gr
LEFT JOIN LATERAL (
  SELECT gpr.full_prefix AS prefix FROM group_prefix_resolved gpr
  WHERE gpr.file_path=gr.file_path AND gpr.bound_var=gr.receiver
  UNION ALL
  SELECT rpb.full_prefix FROM register_prefix_best rpb
  WHERE rpb.register_fn=gr.enclosing_fn
    AND NOT EXISTS (SELECT 1 FROM group_prefix_resolved g2
                    WHERE g2.file_path=gr.file_path AND g2.bound_var=gr.receiver)
  LIMIT 1
) pref ON TRUE
ORDER BY file, start_line, method, path
)__QUACKAPI_SQL__";

// src/sql/quack_from_gin_models.sql
static constexpr const char *k_gin_models =
    R"__QUACKAPI_SQL__(-- quack_from_gin_models — Go struct fields with tags via sitting_duck
-- Placeholder: __REPO__  (app root; C++ expands bare dirs to __REPO__/**/*.go)
-- Returns: model_name, field_name, field_type, is_required, is_optional, has_default,
--          default_expr, file, field_line
WITH
ast AS (
  SELECT * FROM read_ast('__REPO__', 'go', ignore_errors := true, peek := 300)
),
type_specs AS (
  SELECT ts.file_path, ts.name AS model_name, st.node_id AS struct_id
  FROM ast ts
  JOIN ast st ON st.file_path=ts.file_path AND st.parent_id=ts.node_id
             AND st.type='struct_type'
  WHERE ts.type='type_spec' AND ts.name IS NOT NULL
),
nested_structs AS (
  SELECT fd.file_path, fd.name AS nest_field, st.node_id AS struct_id,
         fd.scope.class AS outer_struct_id
  FROM ast fd
  JOIN ast st ON st.file_path=fd.file_path AND st.parent_id=fd.node_id
             AND st.type='struct_type'
  WHERE fd.type='field_declaration' AND fd.name IS NOT NULL
),
struct_names AS (
  SELECT file_path, struct_id, model_name FROM type_specs
  UNION ALL
  SELECT ns.file_path, ns.struct_id,
         COALESCE(ts.model_name,'_anon')||'.'||ns.nest_field
  FROM nested_structs ns
  LEFT JOIN type_specs ts ON ts.file_path=ns.file_path AND ts.struct_id=ns.outer_struct_id
),
fields AS (
  SELECT sn.model_name, f.name AS field_name, f.file_path,
         f.start_line AS field_line,
         (SELECT COALESCE(t.peek,t.name) FROM ast t
          WHERE t.file_path=f.file_path AND t.parent_id=f.node_id
            AND t.type IN ('type_identifier','pointer_type','slice_type','array_type',
                           'map_type','qualified_type','struct_type','interface_type',
                           'channel_type','function_type')
          ORDER BY t.sibling_index LIMIT 1) AS field_type,
         (SELECT trim(both '`' FROM t.peek) FROM ast t
          WHERE t.file_path=f.file_path AND t.parent_id=f.node_id
            AND t.type='raw_string_literal' LIMIT 1) AS tag
  FROM ast f
  JOIN struct_names sn ON sn.file_path=f.file_path AND sn.struct_id=f.scope.class
  WHERE f.type='field_declaration' AND f.name IS NOT NULL AND f.scope.class IS NOT NULL
)
SELECT model_name, field_name, field_type,
  COALESCE(regexp_matches(tag, 'binding:"[^"]*\brequired\b'), false) AS is_required,
  (starts_with(COALESCE(field_type,''), '*')
   OR COALESCE(regexp_matches(tag, '\bomitempty\b'), false)) AS is_optional,
  false AS has_default, CAST(NULL AS VARCHAR) AS default_expr,
  file_path AS file, field_line
FROM fields
WHERE tag IS NOT NULL
  AND (tag LIKE '%json:%' OR tag LIKE '%binding:%' OR tag LIKE '%form:%'
       OR tag LIKE '%uri:%' OR tag LIKE '%header:%' OR tag LIKE '%gorm:%')
ORDER BY model_name, field_line, field_name
)__QUACKAPI_SQL__";

struct EmbeddedSql {
	const char *framework;
	const char *kind;    // "routes" | "models"
	const char *relpath; // path under repo root
	const char *sql;
};

inline const std::vector<EmbeddedSql> &All() {
	static const std::vector<EmbeddedSql> k = {
	    {"fastapi", "routes", "src/sql/quack_from_fastapi_routes.sql", k_fastapi_routes},
	    {"fastapi", "models", "src/sql/quack_from_fastapi_models.sql", k_fastapi_models},
	    {"rails", "routes", "src/sql/quack_from_rails_routes.sql", k_rails_routes},
	    {"rails", "models", "src/sql/quack_from_rails_models.sql", k_rails_models},
	    {"express", "routes", "src/sql/quack_from_express_routes.sql", k_express_routes},
	    {"express", "models", "src/sql/quack_from_express_models.sql", k_express_models},
	    {"gin", "routes", "src/sql/quack_from_gin_routes.sql", k_gin_routes},
	    {"gin", "models", "src/sql/quack_from_gin_models.sql", k_gin_models},
	};
	return k;
}

inline const char *Lookup(const char *framework, const char *kind) {
	for (const auto &e : All()) {
		if (std::string(e.framework) == framework && std::string(e.kind) == kind) {
			return e.sql;
		}
	}
	return nullptr;
}

} // namespace quackapi_from_x_sql
} // namespace duckdb
