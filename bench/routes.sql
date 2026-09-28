-- Stack A: quackapi pure HTTP, every data route hits pgEdge (same as FastAPI).
-- With serve pg_dsn:=… the server executes these via libpq (not ATTACH).

-- serve_quackapi.sh loads postgres, configures its pool, and attaches the shared
-- benchmark PG_DSN as pg before reading this file.

CREATE ROUTE hello GET '/hello' AS
  SELECT 'world' AS msg;

CREATE ROUTE item GET '/items/:id' AS
  SELECT id, name FROM pg.bench_rows WHERE id = $id::INTEGER;

CREATE ROUTE rows_n GET '/rows' AS
  SELECT id, name, value, ts FROM pg.bench_rows ORDER BY id LIMIT $n::INTEGER;

CREATE ROUTE ins POST '/write' AS
  INSERT INTO pg.bench_writes (id, note) VALUES ($id::BIGINT, $note::VARCHAR) RETURNING id;
