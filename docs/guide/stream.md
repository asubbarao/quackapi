# CREATE STREAM — Server-Sent Events (SSE)

Push a live event stream over HTTP with `text/event-stream`. Each result row becomes one SSE event (`id:` + `data:` JSON).

All examples run against `build/release/duckdb -unsigned` with `LOAD quackapi;`.

---

## Basic stream

```sql
CREATE STREAM ticks GET '/ticks' AS
SELECT i AS id, 'tick' AS msg FROM range(3) t(i);
```

```sh
curl -N http://127.0.0.1:8000/ticks
```

Live output:

```text
Content-Type: text/event-stream

id: 0
data: {"id":0,"msg":"tick"}

id: 1
data: {"id":1,"msg":"tick"}

id: 2
data: {"id":2,"msg":"tick"}
```

Multiple rows (including `UNION ALL`) each become an event:

```sql
CREATE STREAM once GET '/once' AS
SELECT 1 AS id, 'a' AS msg
UNION ALL
SELECT 2 AS id, 'b' AS msg;
```

---

## Polling interval

For long-lived “keep selecting” streams, set an interval:

```sql
CREATE STREAM live GET '/live' WITH (interval='1s') AS
SELECT now() AS ts;

-- bare number = seconds
CREATE OR REPLACE STREAM live GET '/live' WITH (interval=2) AS
SELECT 1 AS n;

-- milliseconds
CREATE OR REPLACE STREAM live GET '/live' WITH (interval='250ms') AS
SELECT 1 AS n;
```

```sql
SELECT name, method, pattern, transport, interval_ms, handler
FROM quackapi_streams();
-- transport is always 'sse' today
```

---

## WebSocket — not supported (use SSE)

```sql
CREATE STREAM chat WS '/ws' AS SELECT 1;
-- error: WebSocket is not supported (bundled HTTP library has no Upgrade API)
```

Use **`CREATE STREAM … GET`** for browser EventSource / SSE clients. See also [coming soon](coming-soon.md).

---

## Notes

- Streams are **not** listed in `quackapi_routes()` — use `quackapi_streams()`.
- Path must start with `/`.
- Streams do not take `REQUIRE` auth in the current version.
- Ordinary routes win if both match the same path.

```sql
DROP STREAM ticks;
```

---

## Next

- [Policies](policies.md)  
- [Static files](static-files.md)
