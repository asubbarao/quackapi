-- quack_from_express routes — Express HTTP routes via sitting_duck
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
