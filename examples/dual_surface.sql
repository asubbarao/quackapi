-- Dual surface: same tables, REST + thin GraphQL, both resolve to SQL.
--
--   GET  /users              → CREATE ROUTE handler SELECT
--   POST /gql  { users … }   → CREATE GRAPHQL ROUTE → SELECT cols FROM users
--
-- Run (sequential session — LOAD first):
--   duckdb < examples/dual_surface.sql
-- Or interactive: paste after LOAD quackapi;
--
-- In-process checks use quackapi_request (no TCP). For live HTTP, add:
--   SELECT * FROM quackapi_serve(18090);

LOAD quackapi;

CREATE OR REPLACE TABLE users AS
SELECT 1 AS id, 'ada' AS name
UNION ALL
SELECT 2, 'grace';

CREATE OR REPLACE TABLE posts AS
SELECT 10 AS id, 1 AS user_id, 'hello' AS title;

-- Wire 1: ordinary HTTP — you write the SQL.
CREATE OR REPLACE ROUTE list_users GET '/users' AS
SELECT id, name FROM users ORDER BY id;

CREATE OR REPLACE ROUTE get_user GET '/users/:id' AS
SELECT id, name FROM users WHERE id = $id::INTEGER;

-- Wire 2: thin GraphQL — client picks columns; engine still runs SELECTs.
CREATE OR REPLACE GRAPHQL ROUTE app POST '/gql' FROM users, posts LIMIT 50;

-- REST
SELECT 'REST /users' AS wire, status, decode(body) AS body
FROM quackapi_request('GET', '/users');

SELECT 'REST /users/1' AS wire, status, decode(body) AS body
FROM quackapi_request('GET', '/users/1');

-- GraphQL (same tables)
SELECT 'GQL multi-root' AS wire, status, decode(body) AS body
FROM quackapi_request(
	'POST',
	'/gql',
	'{"query":"{ users { id name } posts { id title } }"}'
);

SELECT 'GQL schema' AS wire, status, decode(body) AS body
FROM quackapi_request('GET', '/gql/schema');

-- Built-in /graphql still works alongside named mounts + REST.
SELECT 'GQL built-in' AS wire, status, decode(body) AS body
FROM quackapi_request(
	'POST',
	'/graphql',
	'{"query":"{ users { name } }"}'
);

SELECT name, method, path, tables, "limit"
FROM quackapi_graphql_routes();
