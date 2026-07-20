-- ============================================================================
-- UNIFIED Node/TypeScript web-framework IR extractor for quackapi
-- sitting_duck (typescript + javascript) → routes + validation models
-- Schema matches extract_python_ir.sql so parquets UNION cleanly.
-- ============================================================================

INSTALL sitting_duck FROM community;
LOAD sitting_duck;
INSTALL parser_tools FROM community;
LOAD parser_tools;
LOAD json;

CREATE OR REPLACE MACRO strip_quotes(s) AS (
  CASE
    WHEN s IS NULL THEN NULL
    WHEN length(s) >= 2 AND (
      (s[1] = '''' AND s[length(s)] = '''') OR
      (s[1] = '"' AND s[length(s)] = '"') OR
      (s[1] = '`' AND s[length(s)] = '`')
    ) THEN substring(s, 2, length(s) - 2)
    ELSE s
  END
);

CREATE OR REPLACE MACRO join_paths(prefix, sub) AS (
  CASE
    WHEN (prefix IS NULL OR prefix = '') AND (sub IS NULL OR sub = '') THEN '/'
    WHEN prefix IS NULL OR prefix = '' THEN
      CASE WHEN starts_with(sub, '/') THEN sub ELSE '/' || sub END
    WHEN sub IS NULL OR sub = '' THEN
      CASE WHEN starts_with(prefix, '/') THEN prefix ELSE '/' || prefix END
    ELSE
      (CASE WHEN starts_with(prefix, '/') THEN prefix ELSE '/' || prefix END)
      || '/'
      || (CASE WHEN starts_with(sub, '/') THEN substring(sub, 2) ELSE sub END)
  END
);

CREATE OR REPLACE MACRO repo_from_path(p) AS (
  CASE
    WHEN p LIKE '%/express-realworld/%' THEN 'express-realworld'
    WHEN p LIKE '%/nestjs-realworld/%' THEN 'nestjs-realworld'
    WHEN p LIKE '%/fastify-realworld/%' THEN 'fastify-realworld'
    WHEN p LIKE '%/koa-realworld/%' THEN 'koa-realworld'
    WHEN p LIKE '%/express-framework/%' THEN 'express-examples'
    WHEN p LIKE '%/nest-framework/%' THEN 'nest-samples'
    WHEN p LIKE '%/koa-examples/%' THEN 'koa-examples'
    WHEN p LIKE '%/zod/%' THEN 'zod'
    WHEN p LIKE '%/class-validator/%' THEN 'class-validator'
    ELSE 'unknown'
  END
);

CREATE OR REPLACE MACRO fw_from_path(p) AS (
  CASE
    WHEN p LIKE '%/express-realworld/%' OR p LIKE '%/express-framework/%' THEN 'express'
    WHEN p LIKE '%/nestjs-realworld/%' OR p LIKE '%/nest-framework/%' THEN 'nestjs'
    WHEN p LIKE '%/fastify-realworld/%' THEN 'fastify'
    WHEN p LIKE '%/koa-realworld/%' OR p LIKE '%/koa-examples/%' THEN 'koa'
    WHEN p LIKE '%/zod/%' OR p LIKE '%/schemas/%' THEN 'zod'
    WHEN p LIKE '%/class-validator/%' THEN 'class-validator'
    ELSE 'unknown'
  END
);

------------------------------------------------------------------------
-- 1. Ingest AST — read_ast path/lang LITERAL-ONLY; tag repo from path
--    Avoid overlapping globs (no separate **/*.dto.ts pass).
------------------------------------------------------------------------
CREATE OR REPLACE TABLE source_ast_raw AS
SELECT a.*
FROM read_ast('/tmp/quackapi_corpus/node/express-realworld/src/**/*.ts', 'typescript', peek := 'full') a
UNION ALL BY NAME
SELECT a.* FROM read_ast('/tmp/quackapi_corpus/node/nestjs-realworld/src/**/*.ts', 'typescript', peek := 'full') a
UNION ALL BY NAME
SELECT a.* FROM read_ast('/tmp/quackapi_corpus/node/fastify-realworld/src/**/*.ts', 'typescript', peek := 'full') a
UNION ALL BY NAME
SELECT a.* FROM read_ast('/tmp/quackapi_corpus/node/koa-realworld/src/**/*.js', 'javascript', peek := 'full') a
UNION ALL BY NAME
SELECT a.* FROM read_ast('/tmp/quackapi_corpus/node/express-framework/examples/**/*.js', 'javascript', peek := 'full') a
UNION ALL BY NAME
SELECT a.* FROM read_ast('/tmp/quackapi_corpus/node/nest-framework/sample/**/*.ts', 'typescript', peek := 'full') a
UNION ALL BY NAME
SELECT a.* FROM read_ast('/tmp/quackapi_corpus/node/koa-examples/**/*.js', 'javascript', peek := 'full') a
UNION ALL BY NAME
SELECT a.* FROM read_ast('/tmp/quackapi_corpus/node/zod/packages/bench/**/*.ts', 'typescript', peek := 'full') a
UNION ALL BY NAME
SELECT a.* FROM read_ast('/tmp/quackapi_corpus/node/zod/packages/zod/src/**/*.ts', 'typescript', peek := 'full') a
UNION ALL BY NAME
SELECT a.* FROM read_ast('/tmp/quackapi_corpus/node/class-validator/sample/**/*.ts', 'typescript', peek := 'full') a
UNION ALL BY NAME
SELECT a.* FROM read_ast('/tmp/quackapi_corpus/node/class-validator/src/**/*.ts', 'typescript', peek := 'full') a
UNION ALL BY NAME
SELECT a.* FROM read_ast('/tmp/quackapi_corpus/node/fastify-realworld/src/schemas/**/*.ts', 'typescript', peek := 'full') a;

-- Dedupe identical (file, node_id) from any accidental overlap
CREATE OR REPLACE TABLE source_ast AS
SELECT
  repo_from_path(file_path) AS repo,
  fw_from_path(file_path) AS repo_framework,
  language AS source_lang,
  *
EXCLUDE (language)
FROM (
  SELECT * EXCLUDE (rn) FROM (
    SELECT *, row_number() OVER (PARTITION BY file_path, node_id ORDER BY start_line) AS rn
    FROM source_ast_raw
  ) WHERE rn = 1
);

CREATE OR REPLACE TABLE ast_stats AS
SELECT repo, repo_framework, source_lang,
       count(*) AS nodes, count(DISTINCT file_path) AS files
FROM source_ast
GROUP BY 1, 2, 3
ORDER BY 1;

------------------------------------------------------------------------
-- 2a. Express / Koa / Fastify method-call routes
------------------------------------------------------------------------
CREATE OR REPLACE TABLE routes_expressish AS
WITH calls AS (
  SELECT
    a.repo,
    a.repo_framework,
    a.file_path,
    a.node_id AS call_id,
    a.start_line,
    lower(a.name) AS method_raw,
    a.peek AS call_peek
  FROM source_ast a
  WHERE a.type = 'call_expression'
    AND lower(a.name) IN ('get','post','put','delete','patch','options','head','all','del')
    AND (
      a.peek LIKE 'router.%'
      OR a.peek LIKE 'app.%'
      OR a.peek LIKE 'server.%'
      OR a.peek LIKE 'fastify.%'
      OR regexp_matches(a.peek, '^[A-Za-z_$][\w$]*\.(get|post|put|delete|patch|options|head|all|del)\(')
    )
    -- drop config.get / map.get noise (no path string first arg)
    AND regexp_matches(a.peek, '\.(get|post|put|delete|patch|options|head|all|del)\(\s*[''"`/]')
),
path_arg AS (
  SELECT
    c.*,
    strip_quotes(s.peek) AS path
  FROM calls c
  JOIN source_ast args
    ON args.file_path = c.file_path
   AND args.parent_id = c.call_id
   AND args.type = 'arguments'
  JOIN source_ast s
    ON s.file_path = c.file_path
   AND s.parent_id = args.node_id
   AND s.type = 'string'
  QUALIFY row_number() OVER (PARTITION BY c.file_path, c.call_id ORDER BY s.node_id) = 1
)
SELECT
  CASE
    WHEN repo_framework IN ('express','fastify','koa') THEN repo_framework
    ELSE 'express'
  END AS framework,
  CASE
    WHEN method_raw = 'del' THEN 'DELETE'
    WHEN method_raw = 'all' THEN 'ANY'
    ELSE upper(method_raw)
  END AS method,
  path,
  coalesce(
    nullif(regexp_extract(call_peek, '(?:get|post|put|delete|patch|options|head|all|del)\([^,]+,\s*(?:async\s+)?([A-Za-z_$][\w$.]*)', 1), ''),
    'anonymous'
  ) AS handler_name,
  file_path AS file,
  start_line,
  repo,
  left(call_peek, 240) AS evidence
FROM path_arg
WHERE path IS NOT NULL
  AND starts_with(path, '/')
  AND length(path) < 200
  AND path NOT LIKE '% %';

------------------------------------------------------------------------
-- 2b. Fastify server.route({ method, url })
------------------------------------------------------------------------
CREATE OR REPLACE TABLE routes_fastify_object AS
WITH route_calls AS (
  SELECT
    a.repo,
    a.file_path,
    a.node_id AS call_id,
    a.start_line,
    a.peek AS call_peek
  FROM source_ast a
  WHERE a.type = 'call_expression'
    AND a.name = 'route'
    AND (a.peek LIKE 'server.route%' OR a.peek LIKE 'fastify.route%' OR a.peek LIKE 'app.route%')
)
SELECT
  'fastify' AS framework,
  upper(coalesce(
    nullif(regexp_extract(call_peek, 'method\s*:\s*[''"]([A-Za-z]+)[''"]', 1), ''),
    'GET'
  )) AS method,
  regexp_extract(call_peek, '(?:url|path)\s*:\s*[''"]([^''"]+)[''"]', 1) AS path,
  coalesce(
    nullif(regexp_extract(call_peek, 'handler\s*:\s*([A-Za-z_$][\w$]*)', 1), ''),
    'handler'
  ) AS handler_name,
  file_path AS file,
  start_line,
  repo,
  left(call_peek, 240) AS evidence
FROM route_calls
WHERE regexp_extract(call_peek, '(?:url|path)\s*:\s*[''"]([^''"]+)[''"]', 1) IS NOT NULL
  AND starts_with(regexp_extract(call_peek, '(?:url|path)\s*:\s*[''"]([^''"]+)[''"]', 1), '/');

------------------------------------------------------------------------
-- 2c. NestJS @Controller + @Get/@Post (decorators siblings of methods)
------------------------------------------------------------------------
CREATE OR REPLACE TABLE routes_nestjs AS
WITH controllers AS (
  SELECT
    d.file_path,
    d.repo,
    cls.node_id AS class_node,
    cls.name AS class_name,
    cls.start_line AS class_start,
    cls.end_line AS class_end,
    coalesce(
      strip_quotes((
        SELECT s.peek
        FROM source_ast args
        JOIN source_ast s
          ON s.file_path = d.file_path AND s.parent_id = args.node_id AND s.type = 'string'
        WHERE args.file_path = d.file_path
          AND args.parent_id = ce.node_id
          AND args.type = 'arguments'
        ORDER BY s.node_id LIMIT 1
      )),
      ''
    ) AS prefix
  FROM source_ast d
  JOIN source_ast ce
    ON ce.file_path = d.file_path AND ce.parent_id = d.node_id
   AND ce.type = 'call_expression' AND ce.name = 'Controller'
  JOIN source_ast cls
    ON cls.file_path = d.file_path AND cls.parent_id = d.parent_id
   AND cls.type = 'class_declaration'
  WHERE d.type = 'decorator'
),
http_decs AS (
  SELECT
    d.repo,
    d.file_path,
    d.node_id AS dec_id,
    d.parent_id AS body_id,
    d.start_line,
    upper(ce.name) AS method,
    coalesce(
      strip_quotes((
        SELECT s.peek
        FROM source_ast args
        JOIN source_ast s
          ON s.file_path = d.file_path AND s.parent_id = args.node_id AND s.type = 'string'
        WHERE args.file_path = d.file_path
          AND args.parent_id = ce.node_id
          AND args.type = 'arguments'
        ORDER BY s.node_id LIMIT 1
      )),
      ''
    ) AS subpath,
    left(d.peek, 240) AS evidence
  FROM source_ast d
  JOIN source_ast ce
    ON ce.file_path = d.file_path AND ce.parent_id = d.node_id
   AND ce.type = 'call_expression'
   AND ce.name IN ('Get','Post','Put','Delete','Patch','Options','Head','All')
  WHERE d.type = 'decorator'
),
with_handler AS (
  SELECT
    h.*,
    (
      SELECT m.name
      FROM source_ast m
      WHERE m.file_path = h.file_path
        AND m.parent_id = h.body_id
        AND m.type = 'method_definition'
        AND m.start_line >= h.start_line
      ORDER BY m.start_line, m.node_id
      LIMIT 1
    ) AS handler_name,
    (
      SELECT c.class_node
      FROM controllers c
      WHERE c.file_path = h.file_path
        AND h.start_line BETWEEN c.class_start AND c.class_end
      ORDER BY c.class_start DESC
      LIMIT 1
    ) AS class_node
  FROM http_decs h
)
SELECT
  'nestjs' AS framework,
  CASE WHEN method = 'ALL' THEN 'ANY' ELSE method END AS method,
  join_paths(c.prefix, wh.subpath) AS path,
  coalesce(wh.handler_name, c.class_name, 'handler') AS handler_name,
  wh.file_path AS file,
  wh.start_line,
  wh.repo,
  wh.evidence
FROM with_handler wh
LEFT JOIN controllers c
  ON c.file_path = wh.file_path AND c.class_node = wh.class_node;

------------------------------------------------------------------------
-- 2d. Unified routes (deduped)
------------------------------------------------------------------------
CREATE OR REPLACE TABLE ir_routes AS
SELECT framework, method, path, handler_name, file, start_line, repo, evidence
FROM (
  SELECT *, row_number() OVER (
    PARTITION BY framework, method, path, file, start_line
    ORDER BY length(evidence) DESC
  ) AS rn
  FROM (
    SELECT * FROM routes_expressish
    UNION ALL BY NAME SELECT * FROM routes_fastify_object
    UNION ALL BY NAME SELECT * FROM routes_nestjs
  )
)
WHERE rn = 1;

------------------------------------------------------------------------
-- 3a. class-validator fields
------------------------------------------------------------------------
CREATE OR REPLACE TABLE class_validator_fields AS
WITH classes AS (
  SELECT
    a.repo,
    a.node_id,
    a.name AS model_name,
    a.file_path,
    a.start_line,
    a.end_line
  FROM source_ast a
  WHERE a.type = 'class_declaration'
    AND a.name IS NOT NULL
),
fields AS (
  SELECT
    f.repo,
    f.file_path,
    f.node_id AS field_node,
    f.name AS field_name,
    f.start_line AS field_line,
    coalesce(
      (
        SELECT t.name
        FROM source_ast ta
        JOIN source_ast t
          ON t.file_path = f.file_path AND t.parent_id = ta.node_id
         AND t.type IN ('predefined_type','type_identifier','generic_type','array_type')
        WHERE ta.file_path = f.file_path
          AND ta.parent_id = f.node_id
          AND ta.type = 'type_annotation'
        ORDER BY t.node_id LIMIT 1
      ),
      'unknown'
    ) AS field_type_raw,
    (
      SELECT list(DISTINCT ce.name)
      FROM source_ast d
      JOIN source_ast ce
        ON ce.file_path = f.file_path AND ce.parent_id = d.node_id
       AND ce.type = 'call_expression'
      WHERE d.file_path = f.file_path
        AND d.parent_id = f.node_id
        AND d.type = 'decorator'
        AND (
          ce.name LIKE 'Is%'
          OR ce.name LIKE 'Validate%'
          OR ce.name IN ('Min','Max','MinLength','MaxLength','Length','Matches','Equals','NotEquals','Contains','NotContains','ArrayMinSize','ArrayMaxSize','Allow')
        )
    ) AS validators
  FROM source_ast f
  WHERE f.type IN ('public_field_definition','property_definition','field_definition')
    AND f.name IS NOT NULL
)
SELECT
  'class-validator' AS framework,
  c.model_name,
  f.field_name,
  f.field_type_raw AS field_type,
  list_contains(coalesce(f.validators, []::VARCHAR[]), 'IsOptional') AS is_optional,
  false AS has_default,
  (
    list_contains(coalesce(f.validators, []::VARCHAR[]), 'IsNotEmpty')
    OR list_contains(coalesce(f.validators, []::VARCHAR[]), 'IsDefined')
    OR (
      len(coalesce(f.validators, []::VARCHAR[])) > 0
      AND NOT list_contains(coalesce(f.validators, []::VARCHAR[]), 'IsOptional')
    )
  ) AS is_required,
  NULL::VARCHAR AS default_expr,
  f.file_path AS file,
  f.field_line,
  f.repo,
  array_to_string(list_sort(coalesce(f.validators, []::VARCHAR[])), ',') AS declared_annotation
FROM fields f
JOIN classes c
  ON c.file_path = f.file_path
 AND f.field_line BETWEEN c.start_line AND c.end_line
WHERE f.validators IS NOT NULL
  AND len(f.validators) > 0
QUALIFY row_number() OVER (
  PARTITION BY f.file_path, f.field_name, f.field_line
  ORDER BY c.start_line DESC
) = 1;

------------------------------------------------------------------------
-- 3b. Zod z.object({ ... }) — pair nodes have empty name; use property_identifier child
------------------------------------------------------------------------
CREATE OR REPLACE TABLE zod_fields AS
WITH zobj AS (
  SELECT
    a.repo,
    a.file_path,
    a.node_id AS call_id,
    a.start_line,
    a.end_line,
    a.peek AS call_peek,
    a.depth AS call_depth,
    coalesce(
      (
        SELECT vd.name
        FROM source_ast vd
        WHERE vd.file_path = a.file_path
          AND vd.type = 'variable_declarator'
          AND vd.start_line <= a.start_line
          AND vd.end_line >= a.start_line
        ORDER BY vd.start_line DESC
        LIMIT 1
      ),
      'zobject_L' || cast(a.start_line AS VARCHAR)
    ) AS model_name
  FROM source_ast a
  WHERE a.type = 'call_expression'
    AND a.name = 'object'
    AND a.peek LIKE 'z.object(%'
),
-- object literal child of z.object(...)
obj_nodes AS (
  SELECT z.*, o.node_id AS obj_id, o.depth AS obj_depth
  FROM zobj z
  JOIN source_ast args
    ON args.file_path = z.file_path AND args.parent_id = z.call_id AND args.type = 'arguments'
  JOIN source_ast o
    ON o.file_path = z.file_path AND o.parent_id = args.node_id AND o.type = 'object'
),
props AS (
  SELECT
    z.repo,
    z.file_path,
    z.model_name,
    z.start_line AS model_line,
    (
      SELECT pi.name
      FROM source_ast pi
      WHERE pi.file_path = z.file_path
        AND pi.parent_id = p.node_id
        AND pi.type = 'property_identifier'
      ORDER BY pi.node_id
      LIMIT 1
    ) AS field_name,
    p.start_line AS field_line,
    p.peek AS prop_peek,
    coalesce(
      nullif(regexp_extract(p.peek, 'z\.([A-Za-z_]+)', 1), ''),
      (
        SELECT ce.name
        FROM source_ast ce
        WHERE ce.file_path = z.file_path
          AND ce.parent_id = p.node_id
          AND ce.type = 'call_expression'
        ORDER BY ce.node_id
        LIMIT 1
      ),
      'unknown'
    ) AS field_type,
    (p.peek ILIKE '%.optional()%' OR p.peek ILIKE '%.nullish()%') AS is_optional,
    (p.peek ILIKE '%.default(%') AS has_default,
    nullif(regexp_extract(p.peek, '\.default\(([^)]*)\)', 1), '') AS default_expr
  FROM obj_nodes z
  JOIN source_ast p
    ON p.file_path = z.file_path
   AND p.parent_id = z.obj_id
   AND p.type = 'pair'
)
SELECT
  'zod' AS framework,
  model_name,
  field_name,
  field_type,
  coalesce(is_optional, false) AS is_optional,
  coalesce(has_default, false) AS has_default,
  (NOT coalesce(is_optional, false) AND NOT coalesce(has_default, false)) AS is_required,
  default_expr,
  file_path AS file,
  field_line,
  repo,
  left(prop_peek, 200) AS declared_annotation
FROM props
WHERE field_name IS NOT NULL AND field_name <> ''
QUALIFY row_number() OVER (
  PARTITION BY file_path, model_name, field_name, field_line
  ORDER BY field_line
) = 1;

------------------------------------------------------------------------
-- 3c. Unified models
------------------------------------------------------------------------
CREATE OR REPLACE TABLE ir_models AS
SELECT
  framework, model_name, field_name, field_type,
  is_optional, has_default, is_required,
  default_expr, file, field_line, repo, declared_annotation
FROM class_validator_fields
UNION ALL BY NAME
SELECT
  framework, model_name, field_name, field_type,
  is_optional, has_default, is_required,
  default_expr, file, field_line, repo, declared_annotation
FROM zod_fields;

------------------------------------------------------------------------
-- 4. Emit
------------------------------------------------------------------------
COPY (SELECT * FROM ir_routes ORDER BY framework, file, start_line, method)
  TO '/tmp/quackapi_corpus/ir_node_routes.parquet' (FORMAT PARQUET);

COPY (SELECT * FROM ir_models ORDER BY framework, model_name, field_name)
  TO '/tmp/quackapi_corpus/ir_node_models.parquet' (FORMAT PARQUET);

COPY (SELECT * FROM ir_routes ORDER BY framework, file, start_line)
  TO '/tmp/quackapi_corpus/ir_node_routes.json' (FORMAT JSON, ARRAY true);

COPY (SELECT * FROM ir_models ORDER BY framework, model_name, field_name)
  TO '/tmp/quackapi_corpus/ir_node_models.json' (FORMAT JSON, ARRAY true);

COPY (SELECT * FROM ast_stats)
  TO '/tmp/quackapi_corpus/ast_stats_node.json' (FORMAT JSON, ARRAY true);

CREATE OR REPLACE TABLE summary_routes AS
SELECT framework, repo, count(*) AS n_routes, count(DISTINCT file) AS n_files
FROM ir_routes GROUP BY 1, 2 ORDER BY 1, 2;

CREATE OR REPLACE TABLE summary_models AS
SELECT framework, repo,
       count(DISTINCT model_name) AS n_models,
       count(*) AS n_fields
FROM ir_models GROUP BY 1, 2 ORDER BY 1, 2;

COPY (SELECT * FROM summary_routes) TO '/tmp/quackapi_corpus/summary_node_routes.json' (FORMAT JSON, ARRAY true);
COPY (SELECT * FROM summary_models) TO '/tmp/quackapi_corpus/summary_node_models.json' (FORMAT JSON, ARRAY true);

-- Node→route→field mapping samples (CreateUserDto ↔ POST /users etc.)
CREATE OR REPLACE TABLE route_field_map AS
SELECT DISTINCT
  r.framework AS route_framework,
  r.method,
  r.path,
  r.handler_name,
  r.repo AS route_repo,
  m.framework AS model_framework,
  m.model_name,
  m.field_name,
  m.field_type,
  m.is_optional,
  m.is_required,
  m.declared_annotation
FROM ir_routes r
JOIN ir_models m
  ON (
    (r.framework = 'nestjs' AND r.path IN ('/users','/users/login','/user') AND m.model_name IN ('CreateUserDto','LoginUserDto','UpdateUserDto','CreateCatDto'))
    OR (r.framework = 'express' AND r.path IN ('/users','/users/login','/user') AND m.model_name ILIKE '%User%')
    OR (r.framework = 'fastify' AND m.framework = 'zod' AND m.model_name ILIKE '%user%')
    OR (r.framework = 'nestjs' AND r.path LIKE '/cats%' AND m.model_name = 'CreateCatDto')
  )
ORDER BY r.framework, r.path, m.model_name, m.field_name;

COPY (SELECT * FROM route_field_map)
  TO '/tmp/quackapi_corpus/node_route_field_map.json' (FORMAT JSON, ARRAY true);

SELECT 'ROUTES' AS kind, count(*)::BIGINT AS n FROM ir_routes
UNION ALL SELECT 'MODELS_FIELDS', count(*) FROM ir_models
UNION ALL SELECT 'MODEL_CLASSES', count(DISTINCT model_name) FROM ir_models
UNION ALL SELECT 'AST_FILES', count(DISTINCT file_path) FROM source_ast
UNION ALL SELECT 'AST_NODES', count(*) FROM source_ast
UNION ALL SELECT 'ZOD_FIELDS', count(*) FROM ir_models WHERE framework='zod'
UNION ALL SELECT 'CV_FIELDS', count(*) FROM ir_models WHERE framework='class-validator';

SELECT * FROM summary_routes;
SELECT * FROM summary_models;
SELECT * FROM route_field_map LIMIT 30;
SELECT framework, method, path, handler_name, repo FROM ir_routes WHERE repo='koa-realworld' LIMIT 15;
SELECT * FROM ir_models WHERE framework='zod' LIMIT 15;
