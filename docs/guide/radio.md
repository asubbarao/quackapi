# radio — a remote DuckDB as a subscriber

[`radio`](https://github.com/Query-Farm/radio) is a DuckDB community extension
that is a **WebSocket client with no server**. `grep -rn "Server" radio/src/`
returns nothing. It dials a URL, buffers what arrives, and hands the buffer
back as ordinary rows.

It registers fourteen SQL functions, all prefixed `radio_`. (A fifteenth
name, `radio_state`, appears in the source: it is the object-cache key for the
per-process singleton, not something SQL can call.)

quackapi is the opposite shape: a WebSocket **server** with no topics. On its
own, `CREATE STREAM … WS` either pushes the rows of one SELECT and closes, or
answers inbound frames. Neither is "many clients are listening; send them
this."

This page is the half that was missing from both. Four table functions make
quackapi a topic broker, so a second DuckDB process — running `radio` and
nothing else — subscribes to a topic here and queries what this server
publishes.

All examples run against `build/release/duckdb -unsigned` with
`LOAD quackapi;`. The subscriber needs `INSTALL radio FROM community;`.

---

## The shape

```sql
-- Subscribe: one cursor per socket, keyed by the request id quackapi mints
-- for the handshake. interval='1ms' is the gap between polls; the poll itself
-- blocks until a message arrives, so a publish is delivered at once.
CREATE STREAM radio_feed WS '/radio/:topic' WITH (interval = '1ms') AS
SELECT message_id, topic, payload, published_at
FROM quackapi_topic_poll($topic, $request_id);

-- Publish from ordinary SQL — here a route, but any SELECT will do.
CREATE ROUTE radio_publish POST '/publish/:topic' AS
SELECT message_id, topic, published_at, subscribers, len(subscribers) AS delivered_to
FROM quackapi_topic_publish($topic, $payload::VARCHAR);

SELECT listen_url FROM quackapi_serve(8000);
```

In a second process:

```sql
LOAD radio;
CALL radio_subscribe('ws://127.0.0.1:8000/radio/room');
CALL radio_sleep(interval '30 seconds');
```

and while that sleeps:

```sh
curl -X POST http://127.0.0.1:8000/publish/room \
  -H 'Content-Type: application/json' \
  --data-binary '{"payload":"hello from quackapi"}'
# [{"message_id":1,"topic":"room","published_at":"…","subscribers":["01…"],"delivered_to":1}]
```

The subscriber now has the message. One result row leaves as one text frame
carrying the same JSON object SSE puts after `data:` — the wire format is
[the WebSocket guide's](websocket.md), unchanged. Land the frames and let a
reader infer the columns rather than reaching for a path selector:

```sql
COPY (
  SELECT decode(message) AS frame
  FROM radio_received_messages()
  WHERE message_type = 'message'
  ORDER BY message_id
) TO 'frames.jsonl' (FORMAT csv, HEADER false, QUOTE '', DELIMITER E'\x01');

CREATE OR REPLACE VIEW raw_frames AS SELECT * FROM read_json('frames.jsonl');
SELECT message_id, topic, payload, published_at FROM raw_frames;
-- 1  room  hello from quackapi  2026-09-19 19:55:01.123
```

## The other direction

A WS handler that binds `$message` answers inbound frames, so a radio client's
`radio_transmit_message` lands in a topic and the published row goes back to
that client as its ack.

```sql
CREATE STREAM radio_ingest WS '/radio-in/:topic' AS
SELECT message_id, topic, published_at
FROM quackapi_topic_publish($topic, $message);
```

```sql
-- subscriber side
CALL radio_subscribe('ws://127.0.0.1:8000/radio-in/inbox');
CALL radio_transmit_message('ws://127.0.0.1:8000/radio-in/inbox', NULL,
                            'hello from radio'::BLOB, 3, interval '10 seconds');
```

```sql
-- server side
SELECT message_id, payload FROM quackapi_topic_messages('inbox');
-- 1  hello from radio
```

**One socket does one direction.** A `$message` handler answers frames and
never polls; a push handler polls and refuses inbound data with Close `1003`.
That is quackapi's existing session model, not something this adds, so a
duplex subscriber is two `radio_subscribe` calls to two paths.

**radio only ever sends binary frames.** `radio_transmit_message` takes a
`BLOB` and `RadioTransmitMessageQueue::senderLoop` calls ixwebsocket's
`sendBinary` unconditionally — there is no text path in it. quackapi used to
refuse a binary frame on a `$message` endpoint outright (Close `1003`,
"binds $message from text frames only"), which meant the reverse direction
could not work with the only DuckDB WebSocket client that exists. It now asks
the *bytes* instead of the opcode: a binary frame whose payload is valid UTF-8
binds `$message` exactly as a text frame would, and one that is not still gets
Close `1003`. That is a deliberate loosening of RFC 6455 §5.6's opcode
distinction, and it is the one change this feature needed outside its own
files.

## The functions

| Function | Rows |
|---|---|
| `quackapi_topic_publish(topic, payload [, retain := 1024])` | `message_id`, `topic`, `published_at`, `subscribers` |
| `quackapi_topic_poll(topic, cursor [, wait_ms := 250])` | `message_id`, `topic`, `payload`, `published_at` |
| `quackapi_topic_messages(topic)` | the retained ring, touching no cursor |
| `quackapi_topics()` | `topic`, `retained`, `retain`, `next_message_id`, `evicted`, `cursors` |

`subscribers` is the list of cursors live at publish time, so `len(subscribers)`
is how many sockets the message reached — the list is the fact, the number is a
view of it. `cursors` is a list of `{cursor, next_message_id, delivered,
skipped}`, one per connected socket.

Ids start at 1 per topic and never repeat within a process.

**A new cursor starts at the topic's tail.** A subscriber gets what is
published after it arrives, not the backlog — that is
`quackapi_topic_messages()`, which no cursor advances. So a publish that races
a connecting subscriber is genuinely lost to it; wait for the cursor to appear
in `quackapi_topics()` before publishing if the ordering matters.

## What is awkward about this

Stated plainly, because none of it is visible from the SQL.

**radio's state is a process-global singleton.** One `Radio` per process, not
per connection and not per database. Two DuckDB connections in the same
subscriber process share one subscription table and one receive queue:
`radio_received_messages()` on either returns both connections' messages.
Unsubscribing on one connection stops the socket for the other.

**quackapi's topic broker is process-global too**, for the same reason: the
publishing connection and the socket sessions are different `Connection`s, and
the topics have to outlive any one of them. Two DuckDB databases in one
process therefore share topics. That is a deliberate match to radio's model,
but it is not the per-database scoping the rest of quackapi's state has
(`QuackapiState::Get(db)`).

**radio's receive queue is lossy on overflow, silently.** Each subscription
holds 1000 messages (`RadioSubscriptionParameters::receive_message_capacity`);
message 1001 evicts message 1. radio counts the evictions internally
(`dropped_unseen` in `RadioReceivedMessageQueueState`) but **no SQL function
returns that counter** — `radio_subscriptions()` reports processed and
transmit counts and not this one. So a subscriber that queries less often than
the server publishes loses messages, and the SELECT that lost them looks
exactly like one that did not.

**quackapi's retained ring is lossy on overflow, and says so.** A topic keeps
`retain` messages (1024 by default; the publisher may raise it). Past that the
oldest is evicted and `quackapi_topics().evicted` counts it. A cursor still
pointing below the oldest retained id is fast-forwarded and its `skipped`
counts the gap. Both numbers are rows you can query; neither is an error.

**radio's build matrix excludes every Windows and every wasm target** while
quackapi ships some of them. radio's distribution workflow passes
`exclude_archs: wasm_mvp;wasm_eh;wasm_threads;windows_amd64_rtools;windows_amd64;windows_amd64_mingw`,
so the published set is Linux and macOS only. quackapi's own
[SUPPORTED_HOSTS.md](../../SUPPORTED_HOSTS.md) includes `windows_amd64`, so on
Windows the server side of this page works and there is no radio client to
talk to it. The two projects' supported-platform sets are not the same set.

**The two do not have to be the same DuckDB version**, and that is the one
place this arrangement is easier than it looks. radio is published for v1.5.4;
quackapi's SUPPORTED_HOSTS.md names v1.5.5. They never share a process — the
subscriber is a second DuckDB with only radio in it, and what crosses between
them is RFC 6455 frames over TCP. A version skew that would make an in-process
`LOAD` fail is simply not in the picture.

**A held socket owns an httplib worker for its whole lifetime**, and sockets
may take at most half of `worker_threads`. With the default 32 that is 16
concurrent subscribers before the upgrade is refused with `503`. A broker that
should hold more subscribers than that wants
`quackapi_serve(worker_threads := …)` sized deliberately — see
[the WebSocket guide's threading section](websocket.md).

**A poll blocks a worker for up to `wait_ms`.** During that window the session
is not reading the socket, so a Close or Ping from the peer waits too. 250ms is
the default trade; lower it if you care more about shutdown latency than about
wakeups. The ceiling is 60s, which is deliberate rather than arbitrary: a
waiter holds a reference to its topic across the wait, and the only thing
stopping the idle sweep from removing that topic is the waiter's own cursor
still being inside the five-minute TTL.

**Cursors are forgotten after five minutes of no polling**, so a long-running
server does not accumulate one entry per socket that ever connected. A cursor
that comes back after that resumes at the tail, exactly as a new one does.

**There is no TLS on this listener.** `wss://` is refused by name on both
sides. Terminate TLS in front of the server and speak `ws://` behind it.

## Proving it

`test/integration/test_radio.py` is the end-to-end test, and it is deliberately
hard to pass by accident: the proof is a **separate OS process** reading back
the exact payload and the exact `message_id` that quackapi returned from
`POST /publish`, not an in-process callback that fired.

```sh
python3 test/integration/test_radio.py --duckdb build/release/duckdb
```

`--subscriber /path/to/stock/duckdb` points the second process at a stock CLI
of the same version, so the subscriber provably has no quackapi linked into it.

It has three ways to fail on purpose, each aimed at a different assertion, and
all three must fail:

```sh
python3 test/integration/test_radio.py --break no-subscriber   # never starts process 2
python3 test/integration/test_radio.py --break dead-endpoint   # process 2 dials a dead port
python3 test/integration/test_radio.py --break no-publish      # process 2 connects; nothing is sent
```

The first two die at the cursor gate. `no-publish` gets a real subscriber all
the way onto the socket and then sends it nothing, so the frame comparison is
what reports the failure — which is the assertion that has to be load-bearing.
