-- test/radio_probe.test.sql
-- LIVE probe for radio community extension (WebSocket client).
-- This exercises subscribe / transmit / listen / received_messages roundtrip.
-- Run with a WS echo server active on the referenced port (use local in 18470-18489 range if no public echo available).
-- Example invocation (echo server pre-started on the referenced port):
--   duckdb -unsigned < test/radio_probe.test.sql

LOAD radio;

-- Subscribe to the echo server (WS or redis-tcp etc supported by radio)
CALL radio_subscribe('ws://localhost:18480');

-- Transmit a message that the echo server should reflect back
CALL radio_transmit_message(
  'ws://localhost:18480',
  NULL,                                 -- channel (for redis etc)
  'radio-probe-roundtrip-OK'::BLOB,
  5,                                    -- max_attempts
  interval '10 seconds'                 -- retry timeout
);

-- Give a moment for network
CALL radio_sleep(interval '250 milliseconds');

-- Block briefly waiting for inbound messages (the echoed payload)
CALL radio_listen(true, interval '3 seconds');

-- Show the round-tripped messages (connection + echoed payload expected)
SELECT
  subscription_id,
  subscription_url,
  message_id,
  message_type,
  receive_time,
  seen_count,
  message
FROM radio_received_messages()
ORDER BY receive_time;

-- Cleanup
CALL radio_unsubscribe('ws://localhost:18480');

-- Also demonstrate flush (no-op here but exercises API)
-- CALL radio_flush(interval '1 second');
