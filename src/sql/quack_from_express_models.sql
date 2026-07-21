-- quack_from_express_models — TS interfaces/classes/zod via sitting_duck
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
