-- quack_from_rails_models — validates + strong params via sitting_duck
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
