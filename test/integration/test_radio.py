#!/usr/bin/env python3
"""radio x quackapi end to end, across two processes.

The claim under test is not "a callback fired". It is: a *separate* DuckDB
process, running the `radio` community extension and nothing of quackapi's,
subscribes to a quackapi WebSocket topic and then reads back, as SQL rows, the
exact payload and the exact message_id that quackapi published over HTTP.

So the assertions are equality against values this script chose or the server
returned, never "is not null" and never "length > 0".

    python3 test/integration/test_radio.py --duckdb build/release/duckdb

--subscriber points the second process at a *stock* DuckDB CLI of the same
version, so the subscriber provably has no quackapi in it. Without it the
publisher's own binary is reused and the report says so.

--break is how you watch this test fail, and each mode aims at a different
assertion. `no-subscriber` never starts the second process and `dead-endpoint`
points it at a port nothing listens on — both die at the cursor gate.
`no-publish` connects the subscriber for real and then sends nothing, so the
frame comparison is what fires. All three must fail. A run that passes under
--break is not measuring anything.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

#! The subscriber sleeps this long with the socket open. The publish happens
#! inside the window, once the server confirms the cursor exists.
SUBSCRIBER_WINDOW_SEC = 12
#! How long to wait for the server to start listening, and for a cursor to show.
STARTUP_TIMEOUT_SEC = 30
CURSOR_TIMEOUT_SEC = 20

TOPIC = "room"
INBOX = "inbox"


class Failure(Exception):
    """An assertion about the wire, not about a mock."""


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def http_json(url: str, payload: dict | None = None, timeout: float = 10.0):
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload).encode()
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=headers)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode())


PUBLISHER_SQL = """
LOAD curl_httpfs;
LOAD httpfs_timeout_retry;
LOAD quackapi;

CREATE OR REPLACE STREAM radio_feed WS '/radio/:topic' WITH (interval = '1ms') AS
SELECT message_id, topic, payload, published_at
FROM quackapi_topic_poll($topic, $request_id);

CREATE OR REPLACE STREAM radio_ingest WS '/radio-in/:topic' AS
SELECT message_id, topic, published_at
FROM quackapi_topic_publish($topic, $message);

CREATE OR REPLACE ROUTE radio_publish POST '/publish/:topic' AS
SELECT message_id, topic, published_at, subscribers, len(subscribers) AS delivered_to
FROM quackapi_topic_publish($topic, $payload::VARCHAR);

CREATE OR REPLACE ROUTE radio_topics GET '/topics' AS
SELECT topic, retained, retain, next_message_id, evicted, cursors
FROM quackapi_topics();

CREATE OR REPLACE ROUTE radio_messages GET '/messages/:topic' AS
SELECT message_id, topic, payload, published_at
FROM quackapi_topic_messages($topic);

SELECT listen_url FROM quackapi_serve({port});
"""

#! The subscriber holds the socket open for the whole window, then lands the
#! frames as a file and lets read_json infer the columns. No path selector
#! touches the payload on the way out.
SUBSCRIBER_SQL = """
LOAD radio;

CALL radio_subscribe('ws://127.0.0.1:{port}/radio/{topic}');
CALL radio_subscribe('ws://127.0.0.1:{port}/radio-in/{inbox}');

CALL radio_sleep(interval '{settle_ms} milliseconds');

CALL radio_transmit_message(
  'ws://127.0.0.1:{port}/radio-in/{inbox}', NULL,
  '{transmit_payload}'::BLOB, 3, interval '10 seconds');

CALL radio_sleep(interval '{window_ms} milliseconds');

COPY (
  SELECT decode(message) AS frame
  FROM radio_received_messages()
  WHERE message_type = 'message'
    AND subscription_url = 'ws://127.0.0.1:{port}/radio/{topic}'
  ORDER BY message_id
) TO '{frames_path}' (FORMAT csv, HEADER false, QUOTE '', DELIMITER E'\\x01');

COPY (
  SELECT subscription_url, message_type::VARCHAR AS message_type, message_id
  FROM radio_received_messages()
  ORDER BY subscription_url, message_id
) TO '{events_path}' (FORMAT csv, HEADER true);
"""


def start_publisher(duckdb: str, port: int, workdir: Path) -> subprocess.Popen:
    script = workdir / "publisher.sql"
    script.write_text(PUBLISHER_SQL.format(port=port))
    log = open(workdir / "publisher.log", "w")
    # stdin stays open so the CLI does not hit EOF and take the listener with it.
    process = subprocess.Popen(
        [duckdb, "-unsigned"],
        stdin=subprocess.PIPE,
        stdout=log,
        stderr=subprocess.STDOUT,
        cwd=str(workdir),
        text=True,
    )
    assert process.stdin is not None
    process.stdin.write(script.read_text())
    process.stdin.flush()
    return process


def wait_for_listener(port: int, deadline: float) -> None:
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.1)
    raise Failure(f"quackapi never listened on 127.0.0.1:{port}")


def cursors_for(base: str, topic: str) -> list[str]:
    try:
        rows = http_json(f"{base}/topics")
    except urllib.error.URLError as error:
        raise Failure(f"GET /topics failed: {error}") from error
    for row in rows:
        if row["topic"] == topic:
            return [cursor["cursor"] for cursor in row["cursors"]]
    return []


def wait_for_cursor(base: str, topic: str, deadline: float) -> list[str]:
    last: list[str] = []
    while time.time() < deadline:
        last = cursors_for(base, topic)
        if last:
            return last
        time.sleep(0.2)
    raise Failure(
        f"no subscriber cursor appeared on topic {topic!r} within "
        f"{CURSOR_TIMEOUT_SEC}s — the second process never opened the socket"
    )


def read_frames(path: Path) -> list[dict]:
    if not path.exists():
        raise Failure(f"the subscriber wrote no frames file at {path}")
    frames = []
    for line in path.read_text().splitlines():
        if line.strip():
            frames.append(json.loads(line))
    return frames


def run(args: argparse.Namespace) -> int:
    duckdb = str(Path(args.duckdb).resolve())
    subscriber_duckdb = str(Path(args.subscriber or args.duckdb).resolve())
    port = args.port or free_port()
    subscriber_port = port if args.brk != "dead-endpoint" else free_port()

    run_id = uuid.uuid4().hex[:12]
    # Quotes and a non-ASCII character on purpose: the frame is JSON, so a
    # payload that survives intact proves the escaping too.
    payload = f'radio "hello" ✓ {run_id}'
    transmit_payload = f"from-radio-{run_id}"

    workdir = Path(tempfile.mkdtemp(prefix="quackapi-radio-"))
    frames_path = workdir / "frames.jsonl"
    events_path = workdir / "events.csv"
    base = f"http://127.0.0.1:{port}"

    print(f"publisher   : {duckdb}")
    print(f"subscriber  : {subscriber_duckdb}")
    if subscriber_duckdb == duckdb:
        print(
            "              (same binary as the publisher — quackapi is linked "
            "into it, though this process runs no server and loads only radio)"
        )
    print(f"port        : {port}")
    print(f"break mode  : {args.brk}")
    print(f"workdir     : {workdir}")
    print(f"payload     : {payload}")

    publisher = start_publisher(duckdb, port, workdir)
    subscriber = None
    try:
        wait_for_listener(port, time.time() + STARTUP_TIMEOUT_SEC)
        print(f"listening   : {base}")

        if args.brk != "no-subscriber":
            subscriber_sql = SUBSCRIBER_SQL.format(
                port=subscriber_port,
                topic=TOPIC,
                inbox=INBOX,
                settle_ms=1500,
                window_ms=SUBSCRIBER_WINDOW_SEC * 1000,
                transmit_payload=transmit_payload,
                frames_path=frames_path.as_posix(),
                events_path=events_path.as_posix(),
            )
            script = workdir / "subscriber.sql"
            script.write_text(subscriber_sql)
            sub_log = open(workdir / "subscriber.log", "w")
            subscriber = subprocess.Popen(
                [subscriber_duckdb, "-unsigned", "-f", str(script)],
                stdout=sub_log,
                stderr=subprocess.STDOUT,
                cwd=str(workdir),
                text=True,
            )
            print(f"subscriber  : pid {subscriber.pid} -> " f"ws://127.0.0.1:{subscriber_port}/radio/{TOPIC}")

        cursors = wait_for_cursor(base, TOPIC, time.time() + CURSOR_TIMEOUT_SEC)
        print(f"cursors     : {cursors}")

        if args.brk == "no-publish":
            # Everything downstream still runs, against what a publish would
            # have produced. The frame assertion is the one that must fire.
            published_row = {
                "message_id": 1,
                "topic": TOPIC,
                "subscribers": cursors,
                "delivered_to": len(cursors),
            }
            print("published   : (skipped by --break no-publish)")
        else:
            published = http_json(f"{base}/publish/{TOPIC}", {"payload": payload})
            if len(published) != 1:
                raise Failure(f"POST /publish returned {len(published)} rows, expected 1")
            published_row = published[0]
            print(f"published   : {json.dumps(published_row)}")

        if published_row["topic"] != TOPIC:
            raise Failure(f"published topic {published_row['topic']!r} != {TOPIC!r}")
        if published_row["subscribers"] != cursors:
            raise Failure(f"publish saw subscribers {published_row['subscribers']} but " f"/topics listed {cursors}")
        if published_row["delivered_to"] != len(cursors):
            raise Failure("delivered_to disagrees with the subscriber list")

        if subscriber is not None:
            subscriber.wait(timeout=SUBSCRIBER_WINDOW_SEC + 60)

        frames = read_frames(frames_path)
        print(f"frames      : {json.dumps(frames)}")
        if len(frames) != 1:
            raise Failure(
                f"the subscriber received {len(frames)} frames on {TOPIC!r}, "
                f"expected exactly the 1 message quackapi published"
            )
        frame = frames[0]
        if frame["payload"] != payload:
            raise Failure(f"payload came back as {frame['payload']!r}, published {payload!r}")
        if frame["message_id"] != published_row["message_id"]:
            raise Failure(f"message_id came back as {frame['message_id']}, published " f"{published_row['message_id']}")
        if frame["topic"] != TOPIC:
            raise Failure(f"topic came back as {frame['topic']!r}, expected {TOPIC!r}")
        print(
            f"RECEIVED    : message_id={frame['message_id']} " f"payload={frame['payload']!r} in a separate OS process"
        )

        # Reverse direction: radio_transmit_message -> $message handler ->
        # quackapi_topic_publish, read back over HTTP as the inbox topic.
        if events_path.exists():
            print("--- radio events seen by the subscriber ---")
            print(events_path.read_text().strip())

        inbox = http_json(f"{base}/messages/{INBOX}")
        inbox_payloads = [row["payload"] for row in inbox]
        if inbox_payloads != [transmit_payload]:
            raise Failure(
                f"inbox holds {inbox_payloads}, expected exactly " f"[{transmit_payload!r}] from radio_transmit_message"
            )
        print(f"TRANSMITTED : {inbox_payloads[0]!r} reached the server from radio")

        print("PASS")
        return 0
    except Failure as failure:
        print(f"FAIL: {failure}")
        return 1
    except Exception as error:  # noqa: BLE001 — a crash is a failing test, not a trace
        print(f"FAIL: {type(error).__name__}: {error}")
        return 1
    finally:
        for process in (subscriber, publisher):
            if process is None or process.poll() is not None:
                continue
            process.kill()
            process.wait(timeout=10)
        for name in ("publisher.log", "subscriber.log"):
            log_path = workdir / name
            if log_path.exists() and log_path.stat().st_size:
                print(f"--- {name} ---")
                print(log_path.read_text()[-4000:])
        if args.keep:
            print(f"kept {workdir}")
        else:
            shutil.rmtree(workdir, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--duckdb",
        default=str(ROOT / "build" / "release" / "duckdb"),
        help="the DuckDB CLI that serves quackapi",
    )
    parser.add_argument(
        "--subscriber",
        default=os.environ.get("QUACKAPI_STOCK_DUCKDB"),
        help="a stock DuckDB CLI of the same version for the radio subscriber",
    )
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument(
        "--break",
        dest="brk",
        choices=("none", "no-subscriber", "dead-endpoint", "no-publish"),
        default="none",
        help="make the test fail on purpose",
    )
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    args = parser.parse_args()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
