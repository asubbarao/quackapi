-- radio × quackapi: a remote DuckDB subscribes to a topic here and queries the
-- messages this server publishes.
--
-- radio (INSTALL radio FROM community) is a WebSocket *client* with no server.
-- quackapi is a WebSocket server with no topics. This file is the missing half:
-- `quackapi_topic_publish` fans a payload out to every socket holding a cursor
-- on the topic, and `quackapi_topic_poll` is what a `CREATE STREAM … WS`
-- endpoint drains to feed those sockets.
--
-- Run the publisher (this file), leave it running:
--   build/release/duckdb -unsigned -f examples/radio.sql
--
-- Then, in a second DuckDB with only radio loaded, run the block at the end.
--
-- Prerequisites (once):
--   INSTALL curl_httpfs FROM community;
--   INSTALL httpfs_timeout_retry FROM community;   -- quackapi_serve requires it

LOAD curl_httpfs;
LOAD httpfs_timeout_retry;
LOAD quackapi;

--===-------------------------------------------------------------------===--
-- The subscribe socket. One poll per execution, keyed by $request_id — the id
-- quackapi mints per handshake — so every connected socket has its own cursor
-- and sees each message once. interval='1ms' is the gap between polls, not the
-- delivery latency: quackapi_topic_poll blocks inside the poll until a message
-- arrives (250ms by default) and a publish wakes it immediately.
--===-------------------------------------------------------------------===--
CREATE OR REPLACE STREAM radio_feed WS '/radio/:topic' WITH (interval = '1ms') AS
SELECT message_id, topic, payload, published_at
FROM quackapi_topic_poll($topic, $request_id);

--===-------------------------------------------------------------------===--
-- The other direction. A handler that binds $message answers inbound frames,
-- so a radio client's radio_transmit_message lands in the topic and the row
-- quackapi_topic_publish returns goes back to that client as its ack.
--===-------------------------------------------------------------------===--
CREATE OR REPLACE STREAM radio_ingest WS '/radio-in/:topic' AS
SELECT message_id, topic, published_at
FROM quackapi_topic_publish($topic, $message);

--===-------------------------------------------------------------------===--
-- Publishing from ordinary SQL: a POST route. `subscribers` is the list of
-- cursors live at publish time, so len(subscribers) is who the message reached.
--===-------------------------------------------------------------------===--
CREATE OR REPLACE ROUTE radio_publish POST '/publish/:topic' AS
SELECT message_id, topic, published_at, subscribers, len(subscribers) AS delivered_to
FROM quackapi_topic_publish($topic, $payload::VARCHAR);

-- What a topic is holding, and where every cursor stands. `evicted` and each
-- cursor's `skipped` are the lossy edges of a bounded ring, reported rather
-- than hidden.
CREATE OR REPLACE ROUTE radio_topics GET '/topics' AS
SELECT topic, retained, retain, next_message_id, evicted, cursors
FROM quackapi_topics();

-- The retained ring itself, which no cursor touches — the backlog a late
-- subscriber does *not* get over the socket.
CREATE OR REPLACE ROUTE radio_messages GET '/messages/:topic' AS
SELECT message_id, topic, payload, published_at
FROM quackapi_topic_messages($topic);

SELECT listen_url FROM quackapi_serve(8000);

-- Nothing yet: no topic exists until something publishes to it or a socket
-- opens a cursor on it.
SELECT topic, retained, cursors FROM quackapi_topics();

--===-------------------------------------------------------------------===--
-- The subscriber, in a second process with only radio loaded:
--
--   LOAD radio;
--   CALL radio_subscribe('ws://127.0.0.1:8000/radio/room');
--   CALL radio_sleep(interval '30 seconds');   -- publish into this window
--
--   -- Land the frames as files first, then let read_json infer the columns:
--   -- the wire format is one JSON object per row, and a reader gives it back
--   -- as message_id / topic / payload / published_at without a path selector.
--   COPY (
--     SELECT decode(message) AS frame
--     FROM radio_received_messages()
--     WHERE message_type = 'message'
--     ORDER BY message_id
--   ) TO 'frames.jsonl' (FORMAT csv, HEADER false, QUOTE '', DELIMITER E'\x01');
--
--   CREATE OR REPLACE VIEW raw_frames AS
--   SELECT * FROM read_json('frames.jsonl');
--
--   SELECT message_id, topic, payload, published_at FROM raw_frames;
--
-- And publishing into it, from a shell:
--
--   curl -X POST http://127.0.0.1:8000/publish/room \
--     -H 'Content-Type: application/json' \
--     --data-binary '{"payload":"hello from quackapi"}'
--   # [{"message_id":1,"topic":"room","published_at":"…","subscribers":["…"],"delivered_to":1}]
--
-- The reverse direction, from that same second process:
--
--   CALL radio_subscribe('ws://127.0.0.1:8000/radio-in/inbox');
--   CALL radio_transmit_message(
--          'ws://127.0.0.1:8000/radio-in/inbox', NULL,
--          'hello from radio'::BLOB, 3, interval '10 seconds');
--   CALL radio_sleep(interval '2 seconds');
--
--   -- then, back on this server:  GET /messages/inbox
--===-------------------------------------------------------------------===--
