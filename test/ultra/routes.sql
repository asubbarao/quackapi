-- Deterministic QuackAPI half of the ultra parity fixture.
-- Every route is SQL and uses the same data and contract as fastapi_app.py.

CREATE TABLE matrix_items AS
SELECT * FROM (VALUES
  (1, 'alice', 12.5::DOUBLE, ['staff']::VARCHAR[]),
  (2, 'bob', 8.0::DOUBLE, ['customer']::VARCHAR[]),
  (3, 'carol', 20.0::DOUBLE, ['staff', 'admin']::VARCHAR[])
) t(id, name, price, tags);

CREATE AUTH matrix_auth AS API_KEY;
SELECT * FROM quackapi_add_api_key('matrix_auth', 'matrix-secret', 'matrix-user');

CREATE ROUTE matrix_health GET '/matrix/health'
  ENVELOPE object
  AS SELECT 'ok' AS status;

CREATE ROUTE matrix_health_head HEAD '/matrix/health'
  AS SELECT 'ok' AS status;

CREATE ROUTE matrix_item GET '/matrix/items/:item_id'
  ENVELOPE object
  EMPTY STATUS 404 BODY '{"detail":"Not Found"}'
  AS SELECT id, name, price, tags
     FROM matrix_items
     WHERE id = $item_id::INTEGER;

CREATE ROUTE matrix_create_item POST '/matrix/items'
  STATUS 201
  ENVELOPE object
  PARAM name VARCHAR
  PARAM price DOUBLE
  PARAM tags VARCHAR DEFAULT NULL
  BODY SCHEMA '{"type":"object","required":[],"additionalProperties":false,"properties":{"name":{"type":"string","minLength":1,"maxLength":32},"price":{"type":"number","minimum":0,"maximum":10000},"tags":{"type":"array","maxItems":4,"items":{"type":"string"}}}}'
  AS SELECT 100 AS id, $name AS name, $price AS price,
            coalesce(from_json($tags::VARCHAR, '["VARCHAR"]'), []::VARCHAR[]) AS tags;

CREATE ROUTE matrix_search GET '/matrix/search'
  PARAM q VARCHAR MIN_LENGTH 1
  PARAM limit INTEGER DEFAULT 10 GE 0 LE 100
  AS SELECT id, name, price, tags
     FROM matrix_items
     WHERE lower(name) LIKE '%' || lower($q::VARCHAR) || '%'
     ORDER BY id
     LIMIT $limit::INTEGER;

CREATE ROUTE matrix_headers GET '/matrix/headers'
  ENVELOPE object
  PARAM x_token HEADER DEFAULT NULL
  PARAM session COOKIE DEFAULT NULL
  AS SELECT $x_token::VARCHAR AS token, $session::VARCHAR AS session;

CREATE ROUTE matrix_auth_route GET '/matrix/auth'
  REQUIRE matrix_auth
  ENVELOPE object
  AS SELECT 'matrix-user' AS subject;

CREATE ROUTE matrix_redirect GET '/matrix/redirect'
  STATUS 307
  AS SELECT '/matrix/health' AS location;

CREATE ROUTE matrix_html GET '/matrix/html'
  AS SELECT '<h1>quack</h1>' AS html;

CREATE ROUTE matrix_text GET '/matrix/text'
  AS SELECT 'quack' AS text;

CREATE ROUTE matrix_object GET '/matrix/object/:item_id'
  ENVELOPE object
  EMPTY STATUS 404 BODY '{"detail":{"code":"missing"}}'
  AS SELECT id, name, price, tags
     FROM matrix_items
     WHERE id = $item_id::INTEGER;

CREATE ROUTE matrix_empty GET '/matrix/empty/:item_id'
  ENVELOPE object
  EMPTY STATUS 404 BODY '{"detail":"item not found"}'
  AS SELECT id, name, price, tags
     FROM matrix_items
     WHERE id = $item_id::INTEGER;

CREATE ROUTE matrix_response_filter GET '/matrix/response-filter'
  ENVELOPE object
  AS SELECT id, name, price, tags
     FROM matrix_items WHERE id = 1;

CREATE ROUTE matrix_compressed GET '/matrix/compressed'
  AS SELECT repeat('x', 4096) AS payload;

CREATE ROUTE matrix_ndjson GET '/matrix/ndjson' FORMAT ndjson
  AS SELECT id, name, price, tags FROM matrix_items ORDER BY id;

CREATE STREAM matrix_stream GET '/matrix/stream'
  AS SELECT i AS id FROM range(3) t(i);

CREATE ROUTE matrix_csv GET '/matrix/csv' FORMAT csv
  AS SELECT id, name FROM matrix_items WHERE id <= 2 ORDER BY id;

CREATE ROUTE matrix_problem GET '/matrix/problem' STATUS 418
  AS SELECT 'short' AS text;
