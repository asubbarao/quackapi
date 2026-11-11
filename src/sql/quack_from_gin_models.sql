-- quack_from_gin_models — Go struct fields with tags via sitting_duck
-- Placeholder: __REPO__  (app root; C++ expands bare dirs to __REPO__/**/*.go)
-- Returns: model_name, field_name, field_type, is_required, is_optional, has_default,
--          default_expr, file, field_line
-- A field's scope.class is the struct_type node in older sitting_duck releases and the
-- enclosing type_spec (or nesting field_declaration) from v1.18.4, so each struct keeps both ids.
WITH
ast AS (
  SELECT * FROM read_ast('__REPO__', 'go', ignore_errors := true, peek := 300)
),
type_specs AS (
  SELECT ts.file_path, ts.name AS model_name, [ts.node_id, st.node_id] AS scope_ids
  FROM ast ts
  JOIN ast st ON st.file_path=ts.file_path AND st.parent_id=ts.node_id
             AND st.type='struct_type'
  WHERE ts.type='type_spec' AND ts.name IS NOT NULL
),
nested_structs AS (
  SELECT fd.file_path, fd.name AS nest_field, [fd.node_id, st.node_id] AS scope_ids,
         fd.scope.class AS outer_scope_id
  FROM ast fd
  JOIN ast st ON st.file_path=fd.file_path AND st.parent_id=fd.node_id
             AND st.type='struct_type'
  WHERE fd.type='field_declaration' AND fd.name IS NOT NULL
),
struct_names AS (
  SELECT file_path, scope_ids, model_name FROM type_specs
  UNION ALL
  -- '_anon' is the published name for a nested struct whose outer struct is unnamed.
  SELECT ns.file_path, ns.scope_ids, concat(coalesce(ts.model_name, '_anon'), '.', ns.nest_field)
  FROM nested_structs ns
  LEFT JOIN type_specs ts ON ts.file_path=ns.file_path AND list_contains(ts.scope_ids, ns.outer_scope_id)
),
field_children AS (
  SELECT t.file_path, t.parent_id AS field_id,
         arg_min(coalesce(t.peek, t.name), t.sibling_index)
           FILTER (WHERE t.type IN ('type_identifier','pointer_type','slice_type','array_type','map_type',
                                    'qualified_type','struct_type','interface_type','channel_type',
                                    'function_type')) AS field_type,
         arg_min(trim(t.peek, '`'), t.sibling_index) FILTER (WHERE t.type='raw_string_literal') AS tag
  FROM ast t
  GROUP BY t.file_path, t.parent_id
),
fields AS (
  SELECT sn.model_name, f.name AS field_name, f.file_path, f.start_line AS field_line,
         c.field_type, c.tag, string_split(c.tag, ' ') AS tag_parts
  FROM ast f
  JOIN struct_names sn ON sn.file_path=f.file_path AND list_contains(sn.scope_ids, f.scope.class)
  JOIN field_children c ON c.file_path=f.file_path AND c.field_id=f.node_id
  WHERE f.type='field_declaration' AND f.name IS NOT NULL
),
binding_options AS (
  SELECT *,
         flatten(list_transform(list_filter(tag_parts, lambda p: starts_with(p, 'binding:"')),
                                lambda p: string_split(trim(p[10:], '"'), ','))) AS binding_values
  FROM fields
  WHERE string_contains_any(tag, ['json:', 'binding:', 'form:', 'uri:', 'header:', 'gorm:'])
)
SELECT model_name, field_name, field_type,
  list_contains(binding_values, 'required') AS is_required,
  CASE WHEN starts_with(field_type, '*') THEN true ELSE contains(tag, 'omitempty') END AS is_optional,
  false AS has_default, CAST(NULL AS VARCHAR) AS default_expr,
  file_path AS file, field_line
FROM binding_options
ORDER BY model_name, field_line, field_name
