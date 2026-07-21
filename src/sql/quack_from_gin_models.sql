-- quack_from_gin_models — Go struct fields with tags via sitting_duck
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
