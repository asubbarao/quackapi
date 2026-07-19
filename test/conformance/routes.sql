-- Conformance fixture routes for quackapi v2 vs FastAPI equivalence.
-- Loaded into an interactive session (FIFO) with build/release/duckdb -unsigned.
-- Params bind from path captures and query string only (v2 has no JSON body binder).

CREATE TABLE users AS
SELECT * FROM (VALUES
  (1, 'alice', 30),
  (2, 'bob', 25),
  (3, 'carol', 40)
) t(id, name, age);

-- params: static GET
CREATE ROUTE health GET '/health' AS
SELECT 'ok' AS status;

-- params: path int
CREATE ROUTE get_user GET '/users/:id' AS
SELECT id, name, age FROM users WHERE id = $id::INTEGER;

-- params: list (no path params)
CREATE ROUTE list_users GET '/users' AS
SELECT id, name, age FROM users ORDER BY id;

-- params: nested path ints
CREATE ROUTE get_post GET '/users/:id/posts/:post_id' AS
SELECT $id::INTEGER AS user_id, $post_id::INTEGER AS post_id;

-- params: required query string
CREATE ROUTE search GET '/search' AS
SELECT id, name, age FROM users
WHERE name ILIKE $q::VARCHAR || '%'
ORDER BY id;

-- params: optional query limit (DEFAULT 10) + LE 100 — FastAPI Query(10, le=100)
CREATE ROUTE search_limit GET '/search_limit'
  PARAM limit INTEGER DEFAULT 10 LE 100
  AS
SELECT id, name, age FROM users
WHERE name ILIKE $q::VARCHAR || '%'
ORDER BY id
LIMIT $limit::INTEGER;

-- params: query string only echo
CREATE ROUTE echo_q GET '/echo' AS
SELECT $q::VARCHAR AS q;

-- params: bool path-ish via query
CREATE ROUTE flag GET '/flag' AS
SELECT $on::BOOLEAN AS on;

-- params: float query
CREATE ROUTE price GET '/price' AS
SELECT $amount::DOUBLE AS amount;

-- methods: POST with query-bound fields (body binder N/A — query stands in for form/query style)
CREATE ROUTE create_user POST '/users' STATUS 201 AS
SELECT $name::VARCHAR AS name, $age::INTEGER AS age;

-- methods: PUT / PATCH / DELETE
CREATE ROUTE put_item PUT '/items/:id' AS
SELECT $id::INTEGER AS id, $q::VARCHAR AS q;

CREATE ROUTE patch_item PATCH '/items/:id' AS
SELECT $id::INTEGER AS id;

CREATE ROUTE del_item DELETE '/items/:id' AS
SELECT $id::INTEGER AS deleted_id;

-- explicit HEAD route (v2 does not auto-register HEAD for GET)
CREATE ROUTE health_head HEAD '/health' AS
SELECT 'ok' AS status;

-- status codes
CREATE ROUTE status_created GET '/status/created' STATUS 201 AS
SELECT 'created' AS text;

CREATE ROUTE status_teapot GET '/status/teapot' STATUS 418 AS
SELECT 'short' AS text;

CREATE ROUTE status_nocontent GET '/status/nocontent' STATUS 204 AS
SELECT '' AS text;

-- content types
CREATE ROUTE page_html GET '/page' AS
SELECT '<h1>hi</h1>' AS html;

CREATE ROUTE page_text GET '/plain' AS
SELECT 'hello' AS text;

CREATE ROUTE page_json GET '/json' AS
SELECT 'world' AS msg, 42 AS n, true AS ok, NULL::INTEGER AS missing;

-- auth: API key
CREATE AUTH site AS API_KEY;
SELECT * FROM quackapi_add_api_key('site', 'k-secret', 'alice');
CREATE ROUTE secure GET '/secure' REQUIRE site AS
SELECT true AS ok, 'alice' AS sub;

-- auth: JWT HS256
CREATE AUTH jwt_auth AS JWT ( SECRET 'conformance-secret' );
CREATE ROUTE jwt_route GET '/jwt' REQUIRE jwt_auth AS
SELECT 'ok' AS status;

-- serve (port injected by harness via .read after SET or direct serve call)
-- Harness appends: SELECT * FROM quackapi_serve(<PORT>);

-- G3: header / cookie params
CREATE ROUTE header_echo GET '/header-echo'
  PARAM x_token HEADER
  AS
SELECT $x_token::VARCHAR AS token;

CREATE ROUTE profile GET '/profile'
  PARAM session COOKIE
  AS
SELECT $session::VARCHAR AS session;

-- G3: redirect + set-cookie responses
CREATE ROUTE old_home GET '/old-home' STATUS 307 AS
SELECT '/new-home' AS location;

CREATE ROUTE login POST '/login' AS
SELECT 'session=sess-abc; Path=/' AS set_cookie, true AS ok;

-- Form-urlencoded + multipart (body binder)
CREATE ROUTE form_submit POST '/form-submit' AS
SELECT $name::VARCHAR AS name, $age::INTEGER AS age;

CREATE ROUTE upload POST '/upload' AS
SELECT $file::VARCHAR AS content, $filename::VARCHAR AS filename;
