-- quack_from_gin routes — Gin HTTP routes via sitting_duck
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
