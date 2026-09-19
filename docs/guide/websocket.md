# CREATE STREAM … WS — WebSockets (RFC 6455)

`CREATE STREAM <name> WS '<path>'` declares a WebSocket endpoint the same way
`CREATE ROUTE` declares an HTTP one. The handshake, framing, ping/pong and close
are quackapi's own: the upgrade is answered from
`QuackapiHttplibServer::process_and_close_socket`, the one place the server still
holds the raw accepted socket.

All examples run against `build/release/duckdb -unsigned` with `LOAD quackapi;`.

---

## Two shapes, chosen by the SQL

A WS handler that **binds `$message`** answers inbound frames. One that does not
**pushes** its rows, exactly as the SSE transport does. There is no flag: the
handler's parameters say which it is, and a contradiction is refused at
`CREATE` time.

```sql
-- push: run the SELECT, send one text frame per row, close
CREATE STREAM ticks WS '/ws/ticks' AS
SELECT i AS id, 'tick' AS msg FROM range(3) t(i);

-- request/response: every inbound text frame binds $message and re-runs the SELECT
CREATE STREAM echo WS '/ws/echo' AS
SELECT $message AS said, length($message) AS len;
```

```sql
SELECT name, transport, binds_message FROM quackapi_streams();
-- ticks  ws  false
-- echo   ws  true
```

`$message` on a `GET` (SSE) stream is an error — nothing would ever supply it.
So is `WITH (interval=…)` on a handler that binds `$message`: a socket that
answers frames has nothing to poll.

## Wire format

One result row leaves as one **text frame** carrying the same JSON object SSE
puts after `data:`. A row larger than 64 KiB leaves as continuation frames and
arrives as one message. If you want an envelope, produce one row:
`SELECT json_group_array(…)`.

Path and query parameters bind from the handshake URL, and `$request_id` binds
to the id echoed in the `X-Request-ID` handshake header.

## Client

`quackapi_ws_connect(url)` is the other half — enough of a client to drive,
debug and test a socket from SQL.

```sql
SELECT seq, opcode, payload, code, frames
FROM quackapi_ws_connect('ws://127.0.0.1:8000/ws/ticks');
```

| named parameter | default | meaning |
|---|---|---|
| `send` | — | text messages to send after the handshake |
| `fragment_size` | `0` | split each message into fragments of this many bytes |
| `ping` | `false` | send a Ping and record the Pong |
| `raw` | — | write these exact bytes instead of framing them |
| `max_frames` | `0` (unlimited) | stop after this many frames |
| `idle_timeout_ms` | `1000` | give up waiting after this long with nothing |

It closes the connection properly on the way out (Close, then the peer's echo),
and it throws rather than guessing if the server does not upgrade or returns the
wrong `Sec-WebSocket-Accept`.

`quackapi_ws_accept(key)` is the handshake derivation on its own —
`base64(SHA-1(key + GUID))`, RFC 6455 §4.2.2.

## What is enforced

| Rule | Response |
|---|---|
| `Sec-WebSocket-Version` is not 13 | `426` + `Sec-WebSocket-Version: 13` |
| not a `GET`, no `Connection: Upgrade`, bad key | `400` |
| path registered, but not as a socket | `400` naming the DDL to write |
| no endpoint at the path | `404` |
| client frame is not masked (§5.3) | Close `1002` |
| reserved bits, bad opcode, fragmented control frame | Close `1002` |
| text or close reason is not UTF-8 (§5.6) | Close `1007` |
| message over 8 MiB, declared or assembled | Close `1009` |
| a push endpoint is sent a message | Close `1003` |

Ping is answered with Pong carrying the same payload. When nothing has arrived
for `read_timeout_sec`, the server sends its own Ping; a second quiet period
with no answer retires the peer with Close `1001`.

## Threading

**A held socket owns one httplib worker for its whole lifetime.** That is the
cost of a thread-per-connection server, and it is the same exhaustion that makes
this server reset connections above its concurrency budget.

So sockets may take at most **half of `worker_threads`** (at least one), and the
upgrade past that is refused with `503` naming the budget — never queued behind
a worker that will not come free. With the default `worker_threads := 32` that
is 16 concurrent sockets and 16 workers left for HTTP. Size it deliberately:

```sql
SELECT * FROM quackapi_serve(8000, worker_threads := 256);
-- 128 concurrent WebSocket sessions, 128 workers for everything else
```

`read_timeout_sec` is also the keep-alive ping cadence, so lowering it detects
dead peers faster and raising it costs less traffic.

## TLS

There is none on this listener. `CREATE STREAM … WSS` and `quackapi_ws_connect('wss://…')`
are both refused by name: terminate TLS in front of the server and speak `ws://`
behind it.
