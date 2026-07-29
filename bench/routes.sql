-- Stack A: quackapi pure HTTP, every data route hits pgEdge (same as FastAPI).
-- With serve pg_dsn:=… the server executes these via libpq (not ATTACH).

INSTALL postgres;
LOAD postgres;
-- ATTACH kept so DuckDB fallback path still works without pg_dsn.
SET pg_use_ctid_scan = false;
SET pg_pool_max_connections = 32;
SET pg_pool_enable_thread_local_cache = true;
ATTACH 'postgresql://admin:password@127.0.0.1:6432/quackbench' AS pg (TYPE postgres);

CREATE ROUTE hello GET '/hello' AS
  SELECT 'world' AS msg;

CREATE ROUTE item GET '/items/:id' AS
  SELECT id, name FROM pg.bench_rows WHERE id = $id::INTEGER;

CREATE ROUTE rows_n GET '/rows' AS
  SELECT id, name, value, ts FROM pg.bench_rows LIMIT $n::INTEGER;

CREATE ROUTE ins POST '/write' AS
  INSERT INTO pg.bench_writes (id, note) VALUES ($id::BIGINT, $note::VARCHAR) RETURNING id;
