# quackapi tests (community extension style)

SQLLogic tests cover handler behavior through DuckDB `unittest`. Separate socket
tests cover behavior that requires a real connection, including disconnects,
write backpressure, payload limits, WebSockets, and database lifetime.

```bash
# build (local extension)
CMAKE_BUILD_PARALLEL_LEVEL=4 MAKEFLAGS=-j4 make release

# run all quackapi SQLLogic tests (same path extension CI uses)
make test_release T='"test/sql/quackapi*"'
```

## How it works

| Piece | Role |
|-------|------|
| `test/sql/*.test` | SQLLogic files (`require quackapi`, query/----) |
| `quackapi_request(method, path [, body] [, headers := MAP])` | In-process TestClient — **same** handler path as TCP serve, **no** listen |

Returns: `status`, `body` (BLOB), `content_type`, `headers` (MAP).  
Text bodies: `decode(body)`. Binary: slice BLOB / `octet_length`.

## Layout

```
test/sql/quackapi_*.test   ← the product tests
```

Use `quackapi_request` for behavior that can be asserted in process. Use the
existing `test/cpp/` and `test/python/` socket tests when the behavior depends on
connection lifetime or transport I/O. The distribution workflow builds and runs
these tests in addition to SQLLogic.

The complete SQL run needs `http_request`, `sitting_duck`, and native `quack`
available through `DUCKDB_TEST_ON_INIT`. Set `QUACKAPI_NETWORK_TESTS=1` for the
cronjob integration and `QUACKAPI_TEST_PG_DSN` to a disposable PostgreSQL database
for the PostgreSQL integration. Without these settings, those two suites skip.

## Examples

```sql
CREATE ROUTE hello GET '/hello' AS SELECT 'world' AS msg;
SELECT status, decode(body) FROM quackapi_request('GET', '/hello');
-- 200  [{"msg":"world"}]

SELECT status FROM quackapi_request(
  'GET', '/echo-rid',
  headers := MAP {'X-Request-ID': 'client-1'}
);
```
