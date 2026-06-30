-- ============================================================================
-- quackapi demo application (pure SQL, registered via register_route + param_schema)
-- Loaded after framework.sql in a scratch session for Tier-1 tests.
-- NO regex, NO LIKE/ILIKE, NO coalesce(col,'') -- structural ops only.
-- ============================================================================

-- Clear any framework-seeded demo routes (app owns the registration as DATA).
TRUNCATE TABLE routes;
TRUNCATE TABLE param_schema;

-- Demo data: initial users (framework may have seeded; ensure here with minimal).
-- Business rows use table; handlers do not hardcode data literals.
CREATE SEQUENCE IF NOT EXISTS users_id_seq START 100;
CREATE TABLE IF NOT EXISTS users (id INTEGER DEFAULT nextval('users_id_seq'), name VARCHAR, age INTEGER);
TRUNCATE TABLE users;
INSERT INTO users (id, name, age) SELECT 1, 'alice', 30 UNION ALL SELECT 2, 'bob', 25 UNION ALL SELECT 3, 'carol', 40;

-- Register the four pillar-exercising endpoints + openapi/docs for Swagger.
-- All via INSERT ... SELECT * FROM register_route(...) macro calls (no VALUES literals for rows).

-- GET /users/{id} : path param int, returns JSON object. 422 on non-int.
INSERT INTO routes SELECT * FROM register_route(
  'get_user',
  'GET',
  '/users/{id}',
  'SELECT to_json(u) AS body FROM users u WHERE u.id = {id}',
  'dynamic',
  'Get a user by id',
  200
);

-- GET /search : query params q (required str), limit (optional int, max constraint).
-- Handler uses structural starts_with (no LIKE), json_group for list.
INSERT INTO routes SELECT * FROM register_route(
  'search',
  'GET',
  '/search',
  'SELECT coalesce(json_group_array(to_json(u)), ''[]'') AS body FROM (SELECT * FROM users WHERE starts_with(lower(name), lower({q})) ORDER BY id LIMIT coalesce({limit}, 100)) u',
  'dynamic',
  'Search users by name prefix with limit cap',
  200
);

-- POST /users : JSON body validated (name str req, age int req) -> write via INSERT RETURNING -> 201.
INSERT INTO routes SELECT * FROM register_route(
  'create_user',
  'POST',
  '/users',
  'INSERT INTO users(name, age) VALUES ({name}, {age}) RETURNING to_json(users) AS body',
  'dynamic',
  'Create a user',
  201
);

-- GET /users : list all users as a JSON array (dynamic, 200).
INSERT INTO routes SELECT * FROM register_route(
  'list_users',
  'GET',
  '/users',
  'SELECT coalesce(json_group_array(to_json(u)), ''[]'') AS body FROM users u',
  'dynamic',
  'List all users',
  200
);

-- GET /events : Server-Sent Events demo (edge #2). kind=stream -> content_type
-- text/event-stream; the C responder flushes each result row as its own
-- `data: ...` chunk with Transfer-Encoding: chunked.
INSERT INTO routes SELECT * FROM register_route(
  'events',
  'GET',
  '/events',
  'SELECT ''tick '' || i AS body FROM range(1, 6) t(i)',
  'stream',
  'SSE event stream demo',
  200
);

-- GET /health : static response, no params, 200 JSON.
INSERT INTO routes SELECT * FROM register_route(
  'health',
  'GET',
  '/health',
  '{"status":"ok"}',
  'static',
  'Health check',
  200
);

-- Also (re)register the framework builtins so /openapi.json and /docs are present for Swagger tests.
INSERT INTO routes SELECT * FROM register_route(
  'openapi',
  'GET',
  '/openapi.json',
  'openapi',
  'openapi',
  'OpenAPI schema',
  200
);

INSERT INTO routes SELECT * FROM register_route(
  'docs',
  'GET',
  '/docs',
  'docs',
  'html',
  'Swagger UI',
  200
);

-- Param schema inserts (structural, explicit; no VALUES literal for "business" config).
INSERT INTO param_schema (route_id, name, location, type, required, constraint_json)
SELECT 'get_user', 'id', 'path', 'int', true, NULL
UNION ALL
SELECT 'search', 'q', 'query', 'string', true, NULL
UNION ALL
SELECT 'search', 'limit', 'query', 'int', false, '{"le":100}'
UNION ALL
SELECT 'create_user', 'name', 'body', 'string', true, NULL
UNION ALL
SELECT 'create_user', 'age', 'body', 'int', true, NULL;

-- Rebuild the precomputed routing structures from THIS app's routes (framework.sql
-- defines _route_index_src / _response_cache_src; we re-materialize after re-seeding
-- so route_index + response_cache reflect the app, not the framework demo seed).
CREATE OR REPLACE TABLE route_index AS SELECT * FROM _route_index_src();
CREATE OR REPLACE TABLE response_cache AS SELECT * FROM _response_cache_src();

-- Self-checks for this app (run at load; use structural checks only).
-- These demonstrate the registered endpoints. Full asserts happen in test session after load.
SELECT status_code, content_type, body IS NULL AS body_null, handler_sql IS NOT NULL AS has_sql FROM handle_request('GET','/users/1','{}','');
SELECT status_code, content_type, body IS NULL AS body_null, handler_sql IS NOT NULL AS has_sql FROM handle_request('GET','/health','{}','');
SELECT status_code, content_type, substr(body,1,30) AS body_preview, handler_sql IS NULL AS no_sql FROM handle_request('GET','/openapi.json','{}','');
SELECT status_code, content_type, starts_with(body, '<!DOCTYPE') AS is_html, handler_sql IS NULL AS no_sql FROM handle_request('GET','/docs','{}','');
