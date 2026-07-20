-- ============================================================================
-- UNIFIED Python web-framework IR extractor for quackapi
-- sitting_duck (literal globs only — no lateral column params)
-- ============================================================================

INSTALL sitting_duck FROM community;
LOAD sitting_duck;
INSTALL parser_tools FROM community;
LOAD parser_tools;
LOAD json;

.timer on

CREATE OR REPLACE MACRO strip_quotes(s) AS (
  CASE
    WHEN s IS NULL THEN NULL
    WHEN starts_with(s, 'r''') OR starts_with(s, 'r"') OR starts_with(s, 'R''') OR starts_with(s, 'R"')
      THEN substring(s, 3, length(s) - 3)
    WHEN length(s) >= 2 AND (
      (s[1] = '''' AND s[length(s)] = '''') OR
      (s[1] = '"' AND s[length(s)] = '"')
    ) THEN substring(s, 2, length(s) - 2)
    ELSE s
  END
);

------------------------------------------------------------------------
-- 1. Ingest AST (literal globs + repo tags)
------------------------------------------------------------------------
CREATE OR REPLACE TABLE source_ast AS
SELECT 'fastapi-realworld' AS repo, 'fastapi' AS repo_framework, a.*
FROM read_ast('/tmp/quackapi_corpus/python/fastapi-realworld/app/**/*.py', 'python', peek := 'full', ignore_errors := true) a
UNION ALL BY NAME
SELECT 'flask-realworld', 'flask', a.*
FROM read_ast('/tmp/quackapi_corpus/python/flask-realworld/conduit/**/*.py', 'python', peek := 'full', ignore_errors := true) a
UNION ALL BY NAME
SELECT 'django-realworld', 'django_drf', a.*
FROM read_ast('/tmp/quackapi_corpus/python/django-realworld/conduit/**/*.py', 'python', peek := 'full', ignore_errors := true) a
UNION ALL BY NAME
SELECT 'drf-realworld', 'django_drf', a.*
FROM read_ast('/tmp/quackapi_corpus/python/drf-realworld/**/*.py', 'python', peek := 'full', ignore_errors := true) a
UNION ALL BY NAME
SELECT 'fastapi-docs', 'fastapi', a.*
FROM read_ast('/tmp/quackapi_corpus/python/fastapi-docs/docs_src/**/*.py', 'python', peek := 'full', ignore_errors := true) a
UNION ALL BY NAME
SELECT 'flask-examples', 'flask', a.*
FROM read_ast('/tmp/quackapi_corpus/python/flask/examples/**/*.py', 'python', peek := 'full', ignore_errors := true) a
UNION ALL BY NAME
SELECT 'drf-framework', 'django_drf', a.*
FROM read_ast('/tmp/quackapi_corpus/python/django-rest-framework/tests/**/*.py', 'python', peek := 'full', ignore_errors := true) a
UNION ALL BY NAME
SELECT 'sqlmodel-docs', 'sqlmodel', a.*
FROM read_ast('/tmp/quackapi_corpus/python/sqlmodel/docs_src/**/*.py', 'python', peek := 'full', ignore_errors := true) a
UNION ALL BY NAME
SELECT 'marshmallow-examples', 'marshmallow', a.*
FROM read_ast('/tmp/quackapi_corpus/python/marshmallow/examples/**/*.py', 'python', peek := 'full', ignore_errors := true) a
UNION ALL BY NAME
SELECT 'pydantic-tests', 'pydantic', a.*
FROM read_ast('/tmp/quackapi_corpus/python/pydantic/tests/**/*.py', 'python', peek := 'full', ignore_errors := true) a
UNION ALL BY NAME
SELECT 'locus', 'pydantic', a.*
FROM read_ast('/Users/aloksubbarao/personal/locus-review-interview/src/**/*.py', 'python', peek := 'full', ignore_errors := true) a;

CREATE OR REPLACE TABLE ast_stats AS
SELECT repo, repo_framework, count(*) AS nodes, count(DISTINCT file_path) AS files
FROM source_ast GROUP BY 1, 2 ORDER BY 1;

SELECT * FROM ast_stats;

------------------------------------------------------------------------
-- 2a. FastAPI: @router.get/post/...
------------------------------------------------------------------------
CREATE OR REPLACE TABLE routes_fastapi AS
WITH dec AS (
  SELECT
    a.repo, a.file_path, a.node_id AS dec_node_id, a.start_line,
    call_n.node_id AS call_id, lower(call_n.name) AS method_raw, call_n.peek AS call_peek
  FROM source_ast a
  JOIN source_ast dec
    ON dec.file_path = a.file_path AND dec.parent_id = a.node_id AND dec.type = 'decorator'
  JOIN source_ast call_n
    ON call_n.file_path = a.file_path AND call_n.parent_id = dec.node_id AND call_n.type = 'call'
  WHERE a.type = 'decorated_definition'
    AND lower(call_n.name) IN ('get','post','put','delete','patch','options','head','trace','api_route','websocket')
),
path_arg AS (
  SELECT d.*, strip_quotes(s.name) AS path
  FROM dec d
  JOIN source_ast al ON al.file_path = d.file_path AND al.parent_id = d.call_id AND al.type = 'argument_list'
  JOIN source_ast s  ON s.file_path = d.file_path AND s.parent_id = al.node_id AND s.type = 'string'
  QUALIFY row_number() OVER (PARTITION BY d.file_path, d.call_id ORDER BY s.sibling_index) = 1
),
handler AS (
  SELECT p.*, fn.name AS handler_name
  FROM path_arg p
  JOIN source_ast fn
    ON fn.file_path = p.file_path AND fn.parent_id = p.dec_node_id AND fn.type = 'function_definition'
)
SELECT
  'fastapi' AS framework,
  CASE WHEN method_raw = 'api_route' THEN 'ANY'
       WHEN method_raw = 'websocket' THEN 'WEBSOCKET'
       ELSE upper(method_raw) END AS method,
  path, handler_name, file_path AS file, start_line, repo, call_peek AS evidence
FROM handler WHERE path IS NOT NULL;

------------------------------------------------------------------------
-- 2b. Flask: @blueprint.route / @app.route
------------------------------------------------------------------------
CREATE OR REPLACE TABLE routes_flask AS
WITH route_calls AS (
  SELECT
    a.repo, a.file_path, a.node_id AS dec_node_id, a.start_line,
    call_n.node_id AS call_id, call_n.peek AS call_peek
  FROM source_ast a
  JOIN source_ast dec
    ON dec.file_path = a.file_path AND dec.parent_id = a.node_id AND dec.type = 'decorator'
  JOIN source_ast call_n
    ON call_n.file_path = a.file_path AND call_n.parent_id = dec.node_id AND call_n.type = 'call'
  WHERE a.type = 'decorated_definition' AND lower(call_n.name) = 'route'
),
path_arg AS (
  SELECT r.*, strip_quotes(s.name) AS path
  FROM route_calls r
  JOIN source_ast al ON al.file_path = r.file_path AND al.parent_id = r.call_id AND al.type = 'argument_list'
  JOIN source_ast s  ON s.file_path = r.file_path AND s.parent_id = al.node_id AND s.type = 'string'
  QUALIFY row_number() OVER (PARTITION BY r.file_path, r.call_id ORDER BY s.sibling_index) = 1
),
methods_kw AS (
  SELECT p.file_path, p.call_id, list(DISTINCT upper(strip_quotes(ms.name))) AS methods
  FROM path_arg p
  JOIN source_ast al ON al.file_path = p.file_path AND al.parent_id = p.call_id AND al.type = 'argument_list'
  JOIN source_ast kw ON kw.file_path = p.file_path AND kw.parent_id = al.node_id
    AND kw.type = 'keyword_argument' AND kw.name = 'methods'
  JOIN source_ast cont ON cont.file_path = p.file_path AND cont.parent_id = kw.node_id
    AND cont.type IN ('tuple','list')
  JOIN source_ast ms ON ms.file_path = p.file_path AND ms.parent_id = cont.node_id AND ms.type = 'string'
  GROUP BY 1, 2
),
handler AS (
  SELECT p.*, fn.name AS handler_name, coalesce(m.methods, ['GET']::VARCHAR[]) AS methods
  FROM path_arg p
  JOIN source_ast fn
    ON fn.file_path = p.file_path AND fn.parent_id = p.dec_node_id AND fn.type = 'function_definition'
  LEFT JOIN methods_kw m ON m.file_path = p.file_path AND m.call_id = p.call_id
)
SELECT 'flask' AS framework, unnest(methods) AS method, path, handler_name,
       file_path AS file, start_line, repo, call_peek AS evidence
FROM handler WHERE path IS NOT NULL;

------------------------------------------------------------------------
-- 2c. Django path/re_path/url
------------------------------------------------------------------------
CREATE OR REPLACE TABLE routes_django_path AS
WITH path_calls AS (
  SELECT a.repo, a.file_path, a.node_id AS call_id, a.start_line, a.name AS call_name, a.peek AS call_peek
  FROM source_ast a
  WHERE a.type = 'call' AND a.name IN ('path', 're_path', 'url')
),
path_arg AS (
  SELECT p.*, strip_quotes(s.name) AS path, s.sibling_index AS path_sib
  FROM path_calls p
  JOIN source_ast al ON al.file_path = p.file_path AND al.parent_id = p.call_id AND al.type = 'argument_list'
  JOIN source_ast s  ON s.file_path = p.file_path AND s.parent_id = al.node_id AND s.type = 'string'
  QUALIFY row_number() OVER (PARTITION BY p.file_path, p.call_id ORDER BY s.sibling_index) = 1
),
handler_arg AS (
  SELECT p.file_path, p.call_id,
    coalesce(
      (SELECT id.name FROM source_ast id
       WHERE id.file_path = p.file_path AND id.parent_id = al.node_id
         AND id.type = 'identifier' AND id.sibling_index > p.path_sib
       ORDER BY id.sibling_index LIMIT 1),
      (SELECT attr.name FROM source_ast attr
       WHERE attr.file_path = p.file_path AND attr.parent_id = al.node_id
         AND attr.type IN ('attribute','call') AND attr.sibling_index > p.path_sib
       ORDER BY attr.sibling_index LIMIT 1)
    ) AS handler_raw
  FROM path_arg p
  JOIN source_ast al ON al.file_path = p.file_path AND al.parent_id = p.call_id AND al.type = 'argument_list'
)
SELECT
  CASE WHEN p.call_name = 'url' THEN 'django_url'
       WHEN p.call_name = 're_path' THEN 'django_re_path'
       ELSE 'django_path' END AS framework,
  'ANY' AS method,
  p.path,
  regexp_replace(coalesce(h.handler_raw, '?'), '\.as_view\(.*', '') AS handler_name,
  p.file_path AS file, p.start_line, p.repo, p.call_peek AS evidence
FROM path_arg p
LEFT JOIN handler_arg h ON h.file_path = p.file_path AND h.call_id = p.call_id
WHERE p.path IS NOT NULL
  AND coalesce(h.handler_raw, '') <> 'include';

------------------------------------------------------------------------
-- 2d. DRF router.register → expanded viewset actions
------------------------------------------------------------------------
CREATE OR REPLACE TABLE routes_drf_register AS
WITH reg AS (
  SELECT a.repo, a.file_path, a.node_id AS call_id, a.start_line, a.peek AS call_peek
  FROM source_ast a
  WHERE a.type = 'call' AND a.name = 'register'
),
args AS (
  SELECT r.*, strip_quotes(s.name) AS prefix,
    (SELECT id.name FROM source_ast id
     WHERE id.file_path = r.file_path AND id.parent_id = al.node_id
       AND id.type = 'identifier' AND id.sibling_index > s.sibling_index
     ORDER BY id.sibling_index LIMIT 1) AS viewset
  FROM reg r
  JOIN source_ast al ON al.file_path = r.file_path AND al.parent_id = r.call_id AND al.type = 'argument_list'
  JOIN source_ast s  ON s.file_path = r.file_path AND s.parent_id = al.node_id AND s.type = 'string'
  QUALIFY row_number() OVER (PARTITION BY r.file_path, r.call_id ORDER BY s.sibling_index) = 1
),
actions AS (
  SELECT * FROM (VALUES
    ('list','GET',''),
    ('create','POST',''),
    ('retrieve','GET','/{pk}'),
    ('update','PUT','/{pk}'),
    ('partial_update','PATCH','/{pk}'),
    ('destroy','DELETE','/{pk}')
  ) AS t(action, method, suffix)
)
SELECT
  'drf_viewset' AS framework, act.method,
  '/' || trim(a.prefix, '/') || act.suffix AS path,
  a.viewset || '.' || act.action AS handler_name,
  a.file_path AS file, a.start_line, a.repo, a.call_peek AS evidence
FROM args a CROSS JOIN actions act
WHERE a.prefix IS NOT NULL AND a.viewset IS NOT NULL;

CREATE OR REPLACE TABLE ir_routes AS
SELECT * FROM routes_fastapi
UNION ALL BY NAME SELECT * FROM routes_flask
UNION ALL BY NAME SELECT * FROM routes_django_path
UNION ALL BY NAME SELECT * FROM routes_drf_register;

SELECT framework, count(*) n FROM ir_routes GROUP BY 1 ORDER BY 1;

------------------------------------------------------------------------
-- 3. Class defs
------------------------------------------------------------------------
CREATE OR REPLACE TABLE class_defs AS
SELECT
  a.repo, a.repo_framework, a.node_id, a.name AS class_name,
  a.file_path, a.start_line, a.end_line,
  list_transform(
    list_filter(a.parameters, lambda p: p.type = 'extends'),
    lambda p: list_reduce(
      string_split(
        list_reduce(string_split(p.name, '['), lambda acc, x, i: CASE WHEN i = 1 THEN x ELSE acc END),
        '.'
      ),
      lambda acc, x, i: x
    )
  ) AS bases
FROM source_ast a
WHERE a.type = 'class_definition' AND a.name IS NOT NULL;

------------------------------------------------------------------------
-- 3a. Pydantic / SQLModel
------------------------------------------------------------------------
CREATE OR REPLACE TABLE pydantic_models_raw AS
WITH RECURSIVE reach AS (
  SELECT class_name, file_path, start_line, end_line, node_id, bases, repo, repo_framework,
         0 AS inheritance_depth,
         CASE WHEN list_contains(bases, 'SQLModel') THEN 'sqlmodel' ELSE 'pydantic' END AS model_kind
  FROM class_defs
  WHERE list_contains(bases, 'BaseModel') OR list_contains(bases, 'SQLModel')
  UNION
  SELECT c.class_name, c.file_path, c.start_line, c.end_line, c.node_id, c.bases, c.repo, c.repo_framework,
         r.inheritance_depth + 1, r.model_kind
  FROM class_defs c
  JOIN reach r ON list_contains(c.bases, r.class_name)
  WHERE r.inheritance_depth < 16
)
SELECT class_name, file_path, start_line, end_line, node_id, bases, repo, repo_framework,
       min(inheritance_depth) AS inheritance_depth, any_value(model_kind) AS model_kind
FROM reach GROUP BY ALL;

CREATE OR REPLACE TABLE pydantic_fields AS
SELECT
  m.model_kind AS framework,
  m.class_name AS model_name,
  a.name AS field_name,
  coalesce(
    nullif(trim(split_part(replace(replace(coalesce(t.peek,''),'Optional[',''),'| None',''),'[',1)), ''),
    'Any'
  ) AS field_type,
  (coalesce(t.peek,'') ILIKE '%Optional[%'
    OR coalesce(t.peek,'') ILIKE '%| None%'
    OR coalesce(t.peek,'') ILIKE '%None |%'
    OR coalesce(rhs.peek,'') = 'None') AS is_optional,
  (eq.node_id IS NOT NULL) AS has_default,
  NOT (eq.node_id IS NOT NULL) AS is_required,
  rhs.peek AS default_expr,
  a.file_path AS file,
  a.start_line AS field_line,
  m.repo,
  t.peek AS declared_annotation
FROM pydantic_models_raw m
JOIN source_ast a
  ON a.file_path = m.file_path AND a.scope.class = m.node_id
 AND a.type = 'assignment' AND a.scope.function IS NULL
LEFT JOIN source_ast t
  ON t.file_path = a.file_path AND t.parent_id = a.node_id AND t.type = 'type'
LEFT JOIN source_ast eq
  ON eq.file_path = a.file_path AND eq.parent_id = a.node_id AND eq.type = '='
LEFT JOIN source_ast rhs
  ON rhs.file_path = a.file_path AND rhs.parent_id = a.node_id
 AND eq.node_id IS NOT NULL AND rhs.sibling_index = eq.sibling_index + 1
WHERE a.name IS NOT NULL AND a.name <> ''
  AND a.name NOT IN ('model_config','Config','Meta')
  AND NOT starts_with(a.name, '_')
  AND (t.peek IS NOT NULL OR eq.node_id IS NOT NULL);

------------------------------------------------------------------------
-- 3b. DRF Serializer
------------------------------------------------------------------------
CREATE OR REPLACE TABLE drf_models_raw AS
WITH RECURSIVE reach AS (
  SELECT class_name, file_path, start_line, end_line, node_id, bases, repo, 0 AS d
  FROM class_defs
  WHERE list_contains(bases, 'Serializer')
     OR list_contains(bases, 'ModelSerializer')
     OR list_contains(bases, 'ListSerializer')
     OR list_contains(bases, 'HyperlinkedModelSerializer')
  UNION
  SELECT c.class_name, c.file_path, c.start_line, c.end_line, c.node_id, c.bases, c.repo, r.d + 1
  FROM class_defs c JOIN reach r ON list_contains(c.bases, r.class_name)
  WHERE r.d < 8
)
SELECT class_name, file_path, start_line, end_line, node_id, bases, repo, min(d) AS inheritance_depth
FROM reach GROUP BY ALL;

CREATE OR REPLACE TABLE drf_fields AS
SELECT
  'drf_serializer' AS framework,
  m.class_name AS model_name,
  a.name AS field_name,
  coalesce(
    (SELECT c.name FROM source_ast c
     WHERE c.file_path = a.file_path AND c.parent_id = a.node_id AND c.type = 'call' LIMIT 1),
    'Field'
  ) AS field_type,
  (a.peek ILIKE '%required=False%' OR a.peek ILIKE '%read_only=True%') AS is_optional,
  (a.peek ILIKE '%default=%' OR a.peek ILIKE '%read_only=True%') AS has_default,
  NOT (a.peek ILIKE '%default=%' OR a.peek ILIKE '%read_only=True%' OR a.peek ILIKE '%required=False%') AS is_required,
  NULL::VARCHAR AS default_expr,
  a.file_path AS file,
  a.start_line AS field_line,
  m.repo,
  a.peek AS declared_annotation
FROM drf_models_raw m
JOIN source_ast a
  ON a.file_path = m.file_path AND a.scope.class = m.node_id
 AND a.type = 'assignment' AND a.scope.function IS NULL
WHERE a.name IS NOT NULL AND a.name <> '' AND a.name <> 'Meta'
  AND NOT starts_with(a.name, '_');

------------------------------------------------------------------------
-- 3c. marshmallow Schema
------------------------------------------------------------------------
CREATE OR REPLACE TABLE marshmallow_models_raw AS
WITH RECURSIVE reach AS (
  SELECT class_name, file_path, start_line, end_line, node_id, bases, repo, 0 AS d
  FROM class_defs WHERE list_contains(bases, 'Schema')
  UNION
  SELECT c.class_name, c.file_path, c.start_line, c.end_line, c.node_id, c.bases, c.repo, r.d + 1
  FROM class_defs c JOIN reach r ON list_contains(c.bases, r.class_name)
  WHERE r.d < 8
)
SELECT class_name, file_path, start_line, end_line, node_id, bases, repo, min(d) AS inheritance_depth
FROM reach GROUP BY ALL;

CREATE OR REPLACE TABLE marshmallow_fields AS
SELECT
  'marshmallow' AS framework,
  m.class_name AS model_name,
  a.name AS field_name,
  coalesce(
    (SELECT c.name FROM source_ast c
     WHERE c.file_path = a.file_path AND c.parent_id = a.node_id AND c.type = 'call' LIMIT 1),
    'Field'
  ) AS field_type,
  (a.peek ILIKE '%required=False%' OR a.peek ILIKE '%allow_none=True%' OR a.peek ILIKE '%dump_only=True%') AS is_optional,
  (a.peek ILIKE '%default=%' OR a.peek ILIKE '%load_default=%' OR a.peek ILIKE '%missing=%' OR a.peek ILIKE '%dump_only=True%') AS has_default,
  (a.peek ILIKE '%required=True%') AS is_required,
  NULL::VARCHAR AS default_expr,
  a.file_path AS file,
  a.start_line AS field_line,
  m.repo,
  a.peek AS declared_annotation
FROM marshmallow_models_raw m
JOIN source_ast a
  ON a.file_path = m.file_path AND a.scope.class = m.node_id
 AND a.type = 'assignment' AND a.scope.function IS NULL
WHERE a.name IS NOT NULL AND a.name <> '' AND a.name <> 'Meta'
  AND NOT starts_with(a.name, '_');

------------------------------------------------------------------------
-- 3d. Unified models
------------------------------------------------------------------------
CREATE OR REPLACE TABLE ir_models AS
SELECT framework, model_name, field_name, field_type,
       coalesce(is_optional,false) AS is_optional,
       coalesce(has_default,false) AS has_default,
       coalesce(is_required, NOT coalesce(has_default,false)) AS is_required,
       default_expr, file, field_line, repo, declared_annotation
FROM pydantic_fields
UNION ALL BY NAME
SELECT framework, model_name, field_name, field_type,
       coalesce(is_optional,false), coalesce(has_default,false),
       coalesce(is_required, NOT coalesce(has_default,false)),
       default_expr, file, field_line, repo, declared_annotation
FROM drf_fields
UNION ALL BY NAME
SELECT framework, model_name, field_name, field_type,
       coalesce(is_optional,false), coalesce(has_default,false),
       CASE WHEN is_required THEN true ELSE NOT coalesce(has_default,false) END,
       default_expr, file, field_line, repo, declared_annotation
FROM marshmallow_fields;

SELECT framework, count(DISTINCT model_name) models, count(*) fields
FROM ir_models GROUP BY 1 ORDER BY 1;

------------------------------------------------------------------------
-- 4. Emit
------------------------------------------------------------------------
COPY (SELECT * FROM ir_routes ORDER BY framework, file, start_line, method)
  TO '/tmp/quackapi_corpus/ir_python_routes.parquet' (FORMAT PARQUET);
COPY (SELECT * FROM ir_models ORDER BY framework, model_name, field_name)
  TO '/tmp/quackapi_corpus/ir_python_models.parquet' (FORMAT PARQUET);
COPY (SELECT * FROM ir_routes ORDER BY framework, file, start_line)
  TO '/tmp/quackapi_corpus/ir_python_routes.json' (FORMAT JSON, ARRAY true);
COPY (SELECT * FROM ir_models ORDER BY framework, model_name, field_name)
  TO '/tmp/quackapi_corpus/ir_python_models.json' (FORMAT JSON, ARRAY true);
COPY (SELECT * FROM ast_stats)
  TO '/tmp/quackapi_corpus/ast_stats_python.json' (FORMAT JSON, ARRAY true);

CREATE OR REPLACE TABLE summary_routes AS
SELECT framework, repo, count(*) AS n_routes, count(DISTINCT file) AS n_files
FROM ir_routes GROUP BY 1, 2 ORDER BY 1, 2;

CREATE OR REPLACE TABLE summary_models AS
SELECT framework, repo, count(DISTINCT model_name) AS n_models, count(*) AS n_fields
FROM ir_models GROUP BY 1, 2 ORDER BY 1, 2;

COPY (SELECT * FROM summary_routes) TO '/tmp/quackapi_corpus/summary_routes.json' (FORMAT JSON, ARRAY true);
COPY (SELECT * FROM summary_models) TO '/tmp/quackapi_corpus/summary_models.json' (FORMAT JSON, ARRAY true);

SELECT 'ROUTES' AS kind, count(*)::BIGINT AS n FROM ir_routes
UNION ALL SELECT 'MODEL_FIELDS', count(*)::BIGINT FROM ir_models
UNION ALL SELECT 'MODEL_CLASSES', count(DISTINCT model_name)::BIGINT FROM ir_models
UNION ALL SELECT 'AST_FILES', count(DISTINCT file_path)::BIGINT FROM source_ast
UNION ALL SELECT 'AST_NODES', count(*)::BIGINT FROM source_ast;

-- sample for PoC: FastAPI route with path param + linked model if any
SELECT * FROM ir_routes
WHERE framework = 'fastapi' AND path LIKE '%{%}%'
ORDER BY repo, start_line
LIMIT 15;

SELECT * FROM ir_models
WHERE framework IN ('pydantic','sqlmodel') AND is_required = true
ORDER BY repo, model_name
LIMIT 15;
