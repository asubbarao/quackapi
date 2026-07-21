-- quack_from_fastapi routes — sitting_duck AST extraction
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
