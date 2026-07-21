-- quack_from_rails routes — Rails routes.rb via sitting_duck
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
