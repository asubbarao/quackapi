# CREATE STREAM — Server-Sent Events (SSE) and WebSocket

Push a live event stream over HTTP with `text/event-stream`. Each result row becomes one SSE event (`id:` + `data:` JSON).
`WS` serves the same rows over a [WebSocket](#websocket).

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
-- transport is 'sse' (GET) or 'ws' (WS)
```

---

## WebSocket

`WS` in place of `GET` serves the same SELECT over a WebSocket. Each row is one
JSON text frame, the same object an SSE event carries in `data:`.

```sql
-- Push: send the rows, then close. With an interval, re-run until the client leaves.
CREATE STREAM ticks_ws WS '/ws/ticks' WITH (interval='1s') AS
SELECT now() AS ts;

-- Reply: a SELECT that uses $message runs once per client message.
CREATE STREAM chat WS '/rooms/:room' AS
SELECT $room AS room, $nick AS nick, upper($message) AS said;
```

```js
const ws = new WebSocket('ws://127.0.0.1:8000/rooms/lobby?nick=ada');
ws.onmessage = (e) => console.log(JSON.parse(e.data)); // {room:"lobby",nick:"ada",said:"HELLO"}
ws.onopen = () => ws.send('hello');
```

- Path captures, query parameters, `$request_id`, `$trace_id` and `$span_id` bind as for SSE.
  `$message` binds the client's text, cast to the type the SELECT expects; a message
  that does not cast is answered with the 422 JSON body and the session continues.
- An upgrade that cannot proceed is answered over HTTP before switching protocols:
  404 (no WS stream at the path), 403 (policy, or a browser `Origin` that is neither
  this server nor in `cors_origins`), 422 (a missing or invalid parameter).
  A plain HTTP request to a WS path gets 426.
- A failed SELECT closes with 1011 and the error as the reason; going past
  `max_response_bytes` for the session closes with 1009.
- The access log gets one row per session, status 101, when it ends.
- Each open session holds one HTTP worker thread (`worker_threads`), as an SSE stream does.

---

## Notes

- Streams are **not** listed in `quackapi_routes()` — use `quackapi_streams()`.
- Path must start with `/`.
- Streams do not take `REQUIRE` auth in the current version, and middleware does not run for them.
- Ordinary routes win if both match the same path.

```sql
DROP STREAM ticks;
```

---

## Next

- [Policies](policies.md)  
- [Static files](static-files.md)
