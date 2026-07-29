# quackapi tests (community extension style)

**Only SQLLogic.** CI runs them via DuckDB `unittest`. No bash harness, no ports.

```bash
# build (local extension)
CMAKE_BUILD_PARALLEL_LEVEL=4 MAKEFLAGS=-j4 make release

# run all quackapi SQLLogic tests (same path extension CI uses)
./build/release/test/unittest "test/sql/quackapi*"
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

Do **not** add `test/http/*.sh`. If a behavior cannot be asserted through
`quackapi_request`, extend the TestClient (headers/body), not a shell script.

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
