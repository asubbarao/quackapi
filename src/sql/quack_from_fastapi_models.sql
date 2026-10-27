-- quack_from_fastapi_models — Pydantic BaseModel/SQLModel fields via sitting_duck
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
field_call_defaults AS (
  SELECT call.node_id, call.file_path,
         bool_or(CASE
           WHEN arg.type = 'keyword_argument' AND arg.name = 'default'
             THEN value.type IS DISTINCT FROM 'ellipsis'
           WHEN arg.type = 'keyword_argument' AND arg.name = 'default_factory'
             THEN value.type NOT IN ('none', 'ellipsis')
           WHEN arg.sibling_index = 1 AND arg.type NOT IN ('keyword_argument', 'ellipsis', ')', 'comment')
             THEN true
           ELSE false
         END) AS has_default
  FROM source_ast call
  JOIN source_ast args
    ON args.file_path = call.file_path AND args.parent_id = call.node_id AND args.type = 'argument_list'
  JOIN source_ast arg
    ON arg.file_path = args.file_path AND arg.parent_id = args.node_id
  LEFT JOIN source_ast value
    ON value.file_path = arg.file_path AND value.parent_id = arg.node_id AND value.sibling_index = 2
  WHERE call.type = 'call' AND list_last(string_split(call.name, '.')) = 'Field'
  GROUP BY ALL
),
field_defaults AS (
  SELECT f.field_id, f.file,
         CASE WHEN rhs.type = 'ellipsis' THEN false
              WHEN fc.node_id IS NOT NULL THEN fc.has_default
              ELSE true END AS has_default,
         rhs.peek AS default_expr
  FROM field_nodes f
  JOIN source_ast eq
    ON eq.file_path = f.file AND eq.parent_id = f.field_id AND eq.type = '='
  LEFT JOIN source_ast rhs
    ON rhs.file_path = f.file AND rhs.parent_id = f.field_id
   AND rhs.sibling_index = eq.sibling_index + 1
  LEFT JOIN field_call_defaults fc ON fc.file_path = rhs.file_path AND fc.node_id = rhs.node_id
)
SELECT
  f.model_name,
  f.field_name,
  coalesce(nullif(trim(ft.field_type), ''), 'Any') AS field_type,
  NOT coalesce(fd.has_default, false) AS is_required,
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
