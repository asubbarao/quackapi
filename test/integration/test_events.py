#!/usr/bin/env python3
"""quackapi + events: a request's lifecycle, proved from outside the process.

Nothing here asserts that a function returned without throwing. Every claim is
a value that left this process, went through an HTTP socket into a quackapi
server, came back out of that server as JSON on the stdin of a *separate*
receiver program, and was read back off disk. The request and its events are
tied together by values both ends can name: the request id the client sent in
X-Request-ID, the connection id and the transaction id the route reported in
its body.

Run it:

    python3 test/integration/test_events.py            # the passing run
    python3 test/integration/test_events.py --sink off # the same suite with the
                                                       # sink turned off; the
                                                       # correlations must fail

The second form is not a convenience. A test whose subject can be removed
without the test noticing has proved nothing, so removing it is part of the
suite.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DUCKDB = ROOT / "build" / "release" / "duckdb"
#: Its own HOME, so ~/.duckdb/extensions is this checkout's and ~/.duckdbrc —
#: which on a developer box LOADs an installed quackapi — is not read at all.
TEST_HOME = ROOT / "build" / "events-home"
#: One directory per run. Nothing in here is ever rewritten or truncated: the
#: bytes the receiver was handed are the record, and a second run makes a
#: second directory rather than touching the first.
RUNS = ROOT / "build" / "events-runs"

#: Written once per run, spawned once per event by the events extension.
RECEIVER = '''#!/usr/bin/env python3
"""One process per event. Append what arrived on stdin, unchanged."""
import os
import sys

raw = sys.stdin.buffer.read()
sink = sys.argv[1]
# O_APPEND + one write(): concurrent receivers cannot interleave a line.
fd = os.open(sink, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
os.write(fd, raw)
os.close(fd)
# Who received it. Proof the JSON crossed a process boundary: these pids are
# not the pid the events payload reports as process_id.
fd = os.open(sink + ".receivers", os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
os.write(fd, ("%d %d\\n" % (os.getpid(), len(raw))).encode())
os.close(fd)
'''

#: quackapi_serve refuses to serve without curl_httpfs and httpfs_timeout_retry
#: whatever this suite is about, and read_json needs json. events is the
#: subject. Value is the FROM clause INSTALL needs.
COMPANIONS = {
    "events": " FROM community",
    "curl_httpfs": " FROM community",
    "httpfs_timeout_retry": " FROM community",
    "json": "",
}

FULL_TYPES = [
    "query_begin",
    "query_end",
    "transaction_begin",
    "transaction_commit",
    "transaction_rollback",
]


class Failure(Exception):
    pass


def check(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def check_equal(actual, expected, what: str) -> None:
    if actual != expected:
        raise Failure(f"{what}: expected {expected!r}, got {actual!r}")


# ---------------------------------------------------------------------------
# duckdb
# ---------------------------------------------------------------------------


def duck_env() -> dict:
    env = os.environ.copy()
    env["HOME"] = str(TEST_HOME)
    return env


def duck_json(sql: str) -> list:
    """Run one statement in a throwaway :memory: database, rows back as JSON."""
    proc = subprocess.run(
        [str(DUCKDB), "-json", "-c", sql],
        cwd=str(ROOT),
        env=duck_env(),
        capture_output=True,
        text=True,
        timeout=120,
    )
    if proc.returncode != 0:
        raise Failure(f"duckdb failed: {proc.stderr.strip()}\nSQL: {sql}")
    out = proc.stdout.strip()
    return json.loads(out) if out else []


def prepare_home() -> None:
    """The companion this suite is about, in a directory this suite owns.

    A test fixture may fetch its own dependency; quackapi may not — its gate
    LOADs and never INSTALLs, which is the behaviour proved in test_gate.
    """
    TEST_HOME.mkdir(parents=True, exist_ok=True)
    wanted = dict(COMPANIONS)
    names_sql = ", ".join("'" + name + "'" for name in wanted)
    installed = duck_json(
        "SELECT array_agg(DISTINCT extension_name) AS names, len(names) AS n "
        f"FROM duckdb_extensions() WHERE extension_name IN ({names_sql}) AND installed"
    )
    have = installed[0]["names"] if installed and installed[0]["names"] else []
    for extension, repository in wanted.items():
        if extension in have:
            continue
        try:
            duck_json(f"INSTALL {extension}{repository}")
        except Failure as exc:
            raise Failure(
                f"the '{extension}' extension is not installed under {TEST_HOME} and could " f"not be fetched: {exc}"
            ) from exc


def check_binary() -> None:
    check(DUCKDB.exists(), f"no duckdb binary at {DUCKDB} — run `make release` first")
    # quackapi is statically linked into this binary, so there is no installed
    # copy to load by accident. This setting only exists in a build that has
    # src/quackapi_events.cpp in it: a binary from before this work fails here
    # instead of passing a suite it never ran.
    settings = duck_json(
        "SELECT array_agg(DISTINCT name) AS names, len(names) AS n FROM duckdb_settings() "
        "WHERE name IN ('quackapi_events', 'quackapi_events_types', 'quackapi_events_async')"
    )
    names = sorted(settings[0]["names"]) if settings and settings[0]["names"] else []
    check_equal(
        names,
        ["quackapi_events", "quackapi_events_async", "quackapi_events_types"],
        f"{DUCKDB} does not carry the quackapi_events settings — it predates this change",
    )


# ---------------------------------------------------------------------------
# the server
# ---------------------------------------------------------------------------


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


# /obs hands the caller the two keys it needs to find its own events, straight
# off the connection the request is being answered on. The second POST /add
# violates the primary key: the handler's transaction rolls back and the client
# is told nothing but 500.
#
# No SQL comments in here: quackapi's CREATE ROUTE parser extension does not
# survive a leading `--` line in the same script (pre-existing, unrelated).
ROUTES = """
CREATE TABLE ledger(id INTEGER PRIMARY KEY);
CREATE ROUTE obs GET '/obs' AS
  SELECT connection_id, transaction_id, session_name FROM quackapi_events();
CREATE ROUTE add POST '/add' AS INSERT INTO ledger VALUES (1) RETURNING id;
"""


class Server:
    def __init__(self, sink_setting: str, types: list, run_dir: Path, name: str):
        self.port = free_port()
        self.run_dir = run_dir
        self.name = name
        self.log = (run_dir / f"{name}.server.log").open("wb")
        types_sql = "[" + ", ".join("'" + t + "'" for t in types) + "]"
        sql = (
            f"SET quackapi_events = '{sink_setting}';\n"
            f"SET quackapi_events_types = {types_sql};\n"
            f"{ROUTES}\n"
            f"SELECT * FROM quackapi_serve({self.port}, host := '127.0.0.1', "
            f"access_log := false, block := true);\n"
        )
        (run_dir / f"{name}.server.sql").write_text(sql)
        self.proc = subprocess.Popen(
            [str(DUCKDB), "-c", sql],
            cwd=str(ROOT),
            env=duck_env(),
            stdout=self.log,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
        )

    def wait_ready(self, timeout: float = 30.0) -> None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise Failure(
                    f"server {self.name} exited with {self.proc.returncode} before listening; "
                    f"see {self.run_dir / (self.name + '.server.log')}"
                )
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.5):
                    return
            except OSError:
                time.sleep(0.1)
        raise Failure(f"server {self.name} never listened on {self.port}")

    def request(self, method: str, path: str, request_id: str):
        req = urllib.request.Request(
            f"http://127.0.0.1:{self.port}{path}", method=method, data=b"{}" if method == "POST" else None
        )
        req.add_header("X-Request-ID", request_id)
        if method == "POST":
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=30) as res:
                return res.status, res.headers, res.read()
        except urllib.error.HTTPError as err:
            return err.code, err.headers, err.read()

    def stop(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=15)
        self.log.close()

    def exit_output(self) -> str:
        return (self.run_dir / f"{self.name}.server.log").read_text()


# ---------------------------------------------------------------------------
# reading the sink
# ---------------------------------------------------------------------------

#: read_json over the raw JSONL the receivers wrote, whole. The named columns
#: the events extension emits are the schema; nothing is picked out of a
#: string with a selector.
#:
#: read_json(path, format := 'newline_delimited', sample_size := -1,
#:           records := 'auto' (default), columns := NULL (default: inferred),
#:           maximum_object_size := 16777216 (default), ignore_errors := false
#:           (default), union_by_name := false (default), filename := false
#:           (default), hive_partitioning := false (default),
#:           auto_detect := true (default), maximum_depth := -1 (default),
#:           dateformat/timestampformat := 'iso' (default), compression :=
#:           'auto' (default))
#: sample_size := -1 reads every record before fixing the schema — the default
#: 20480 would decide 'has_error' does not exist off a prefix that lacks it and
#: drop it from every later row.
SINK_SQL = """
WITH raw AS (
  SELECT * FROM read_json('{sink}', format := 'newline_delimited', sample_size := -1)
)
SELECT session_name,
       connection_id,
       transaction_id,
       list_sort(array_agg(event)) AS events,
       len(events) AS n,
       list_sort(array_agg(DISTINCT has_error)) AS error_flags,
       len(error_flags) AS error_flag_n
FROM raw
GROUP BY session_name, connection_id, transaction_id
ORDER BY session_name, connection_id, transaction_id
"""


def read_sink(sink: Path) -> list:
    if not sink.exists():
        return []
    try:
        return duck_json(SINK_SQL.format(sink=sink))
    except (Failure, json.JSONDecodeError):
        # A receiver may be mid-append. The caller polls; a torn read is not a
        # verdict.
        return []


def await_sink(sink: Path, request_id: str, wanted: set, timeout: float = 20.0) -> list:
    """Rows for one request id, once every wanted event type has arrived."""
    deadline = time.time() + timeout
    rows: list = []
    while time.time() < deadline:
        rows = [row for row in read_sink(sink) if row["session_name"] == request_id]
        arrived = {event for row in rows for event in row["events"]}
        if wanted <= arrived:
            # Settle: a later event of the same request must not race the
            # assertions that follow.
            time.sleep(0.5)
            return [row for row in read_sink(sink) if row["session_name"] == request_id]
        time.sleep(0.1)
    arrived = {event for row in rows for event in row["events"]}
    raise Failure(
        f"request {request_id}: waited {timeout}s for {sorted(wanted)} in {sink}; "
        f"what arrived was {sorted(arrived)}"
    )


# ---------------------------------------------------------------------------
# the cases
# ---------------------------------------------------------------------------


def transaction_groups(rows: list) -> dict:
    """The sink rows of one request, by transaction id.

    query_end always lands under transaction_id 0: events reads the id after
    the transaction has already closed, so the id is gone by then. Everything
    else carries the real one.
    """
    return {row["transaction_id"]: row for row in rows}


def case_success(server: Server, sink: Path) -> None:
    """The request the route answered, and the events that request produced."""
    rid = "qa-events-success-1"
    status, headers, body = server.request("GET", "/obs", rid)
    check_equal(status, 200, "GET /obs status")
    check_equal(headers.get("X-Request-ID"), rid, "X-Request-ID echoed")
    row = json.loads(body.decode())[0]
    check_equal(row["session_name"], rid, "the route sees its own request id as the session name")
    connection_id = row["connection_id"]
    transaction_id = row["transaction_id"]

    rows = await_sink(sink, rid, {"query_begin", "query_end", "transaction_begin", "transaction_commit"})
    check_equal(
        sorted({r["connection_id"] for r in rows}),
        [connection_id],
        "every event of this request is on the connection the route reported",
    )
    groups = transaction_groups(rows)
    check(
        transaction_id in groups,
        f"transaction {transaction_id} from the route body is in the sink; ids there are {sorted(groups)}",
    )
    check_equal(
        groups[transaction_id]["events"],
        ["query_begin", "transaction_begin", "transaction_commit"],
        f"the lifecycle of the handler transaction {transaction_id}",
    )
    check_equal(groups[transaction_id]["n"], 3, "event count for the handler transaction")
    check(0 in groups, f"the query_end events of {rid} are in the sink; ids there are {sorted(groups)}")
    check_equal(groups[0]["error_flags"], [False], "nothing this request ran reported an error")


def case_rollback(server: Server, sink: Path) -> None:
    """A rolled-back transaction, correlated to a request whose body says nothing."""
    first = "qa-events-insert-ok"
    status, _, body = server.request("POST", "/add", first)
    check_equal(status, 200, "first POST /add status")
    check_equal(json.loads(body.decode()), [{"id": 1}], "first POST /add body")

    second = "qa-events-insert-rollback"
    status, headers, body = server.request("POST", "/add", second)
    check_equal(status, 500, "second POST /add status")
    check_equal(headers.get("X-Request-ID"), second, "X-Request-ID echoed on the failure")
    check_equal(
        json.loads(body.decode()),
        {"detail": "Internal Server Error"},
        "the failure tells the client nothing — which is why the sink has to",
    )

    committed = await_sink(sink, first, {"transaction_commit"})
    check_equal(
        sorted({e for r in committed for e in r["events"]}),
        ["query_begin", "query_end", "transaction_begin", "transaction_commit"],
        f"{first} committed and never rolled back",
    )
    check_equal(transaction_groups(committed)[0]["error_flags"], [False], f"{first} reported no error")

    # Nothing in the 500 names a connection or a transaction. The request id
    # the client sent is the only handle it has, and the stamp put it on every
    # event this request produced.
    rolled_back = await_sink(sink, second, {"transaction_rollback"})
    check_equal(
        len({r["connection_id"] for r in rolled_back}),
        1,
        f"{second} was answered on one connection, got {sorted({r['connection_id'] for r in rolled_back})}",
    )
    failed = [r for r in rolled_back if "transaction_rollback" in r["events"]]
    check_equal(len(failed), 1, f"exactly one rolled-back transaction for {second}")
    # events fires transaction_rollback twice for one failed statement — once
    # carrying the error, once not. Recorded as it is, not as it ought to be.
    check_equal(
        failed[0]["events"],
        ["query_begin", "transaction_begin", "transaction_rollback", "transaction_rollback"],
        f"the lifecycle of the transaction {second} rolled back",
    )
    check_equal(
        failed[0]["error_flags"],
        [False, True, None],
        "the two rollbacks disagree about the error flag, and query_begin/transaction_begin carry none",
    )
    check_equal(
        transaction_groups(rolled_back)[0]["error_flags"],
        [False, True],
        f"one of {second}'s statements ended in error and the others did not",
    )
    check(
        failed[0]["connection_id"] not in {r["connection_id"] for r in committed},
        "the failed request ran on its own connection, not the one that committed",
    )


def case_narrowed(run_dir: Path, sink_setting: str) -> None:
    """events_types narrowed: query_begin is deliberately not delivered."""
    sink = run_dir / "narrowed.jsonl"
    server = Server(
        sink_setting if sink_setting == "off" else f"{sink_setting} {sink}",
        ["query_end", "transaction_begin", "transaction_commit"],
        run_dir,
        "narrowed",
    )
    try:
        server.wait_ready()
        rid = "qa-events-narrowed-1"
        status, _, body = server.request("GET", "/obs", rid)
        check_equal(status, 200, "GET /obs status under a narrowed events_types")
        row = json.loads(body.decode())[0]
        check_equal(row["session_name"], rid, "the route names its request under a narrowed list too")
        transaction_id = row["transaction_id"]

        rows = await_sink(sink, rid, {"query_end", "transaction_commit"})
        check_equal(
            sorted({e for r in rows for e in r["events"]}),
            ["query_end", "transaction_begin", "transaction_commit"],
            "query_begin is absent because it was not asked for",
        )
        groups = transaction_groups(rows)
        check(
            transaction_id in groups,
            f"transaction {transaction_id} from the route body is in the sink; ids there are {sorted(groups)}",
        )
        check_equal(
            groups[transaction_id]["events"],
            ["transaction_begin", "transaction_commit"],
            "the handler transaction, minus the event that was not asked for",
        )
        check_equal(groups[transaction_id]["n"], 2, "event count under the narrowed list")
    finally:
        server.stop()


def case_sink_is_down(run_dir: Path, destination: str, name: str) -> None:
    """The event sink is broken. The request is not."""
    server = Server(destination, FULL_TYPES, run_dir, name)
    try:
        server.wait_ready()
        rid = f"qa-events-{name}"
        status, headers, body = server.request("GET", "/obs", rid)
        check_equal(status, 200, f"GET /obs status with a {name} handler")
        check_equal(headers.get("X-Request-ID"), rid, f"X-Request-ID echoed with a {name} handler")
        row = json.loads(body.decode())[0]
        check_equal(row["session_name"], rid, f"the route still names its request with a {name} handler")

        status, _, body = server.request("POST", "/add", f"{rid}-write")
        check_equal(status, 200, f"POST /add status with a {name} handler")
        check_equal(json.loads(body.decode()), [{"id": 1}], f"POST /add body with a {name} handler")
    finally:
        server.stop()


def case_gate(run_dir: Path, receiver: Path) -> None:
    """A configured handler with no events extension is a refusal to serve."""
    # Everything quackapi_serve needs except the one under test, so the
    # refusal names events rather than whichever gate happens to run first.
    gate_home = run_dir / "no-events-home"
    source = sorted(TEST_HOME.glob(".duckdb/extensions/*/*"))
    check(len(source) == 1, f"expected one extension directory under {TEST_HOME}, found {source}")
    target = gate_home / source[0].relative_to(TEST_HOME)
    target.mkdir(parents=True, exist_ok=True)
    for artifact in source[0].iterdir():
        if artifact.name.startswith("events."):
            continue
        shutil.copy2(artifact, target / artifact.name)
    sink = run_dir / "gate.jsonl"
    port = free_port()
    sql = (
        f"SET quackapi_events = '{receiver} {sink}';\n"
        f"SELECT * FROM quackapi_serve({port}, host := '127.0.0.1', access_log := false, block := true);\n"
    )
    env = os.environ.copy()
    env["HOME"] = str(gate_home)
    proc = subprocess.run([str(DUCKDB), "-c", sql], cwd=str(ROOT), env=env, capture_output=True, text=True, timeout=120)
    (run_dir / "gate.server.log").write_text(proc.stdout + proc.stderr)
    combined = proc.stdout + proc.stderr
    check_equal(proc.returncode, 1, "quackapi_serve should exit non-zero rather than serve without events")
    check(
        "requires the 'events' extension" in combined,
        f"quackapi_serve should refuse to serve without events; it said: {combined.strip()[:400]}",
    )
    check(
        "INSTALL events FROM community" in combined,
        "the refusal should name the command that fixes it",
    )
    check(not sink.exists(), "nothing was written when the companion was missing")


def case_receivers_are_separate_processes(sink: Path) -> None:
    """The JSON crossed a process boundary — these pids are not duckdb's."""
    receipts = Path(str(sink) + ".receivers")
    check(receipts.exists(), f"no receiver log at {receipts}")
    pids = {int(line.split()[0]) for line in receipts.read_text().splitlines() if line.strip()}
    emitters = duck_json(
        "SELECT array_agg(DISTINCT process_id) AS pids, len(pids) AS n "
        f"FROM read_json('{sink}', format := 'newline_delimited', sample_size := -1)"
    )
    emitter_pids = set(emitters[0]["pids"]) if emitters and emitters[0]["pids"] else set()
    check_equal(len(emitter_pids), 1, f"one duckdb process emitted these events, got {sorted(emitter_pids)}")
    check(len(pids) > 1, f"each event should have its own receiver process, saw {sorted(pids)}")
    check_equal(
        sorted(pids & emitter_pids),
        [],
        "no receiver ran inside the duckdb process that emitted the events",
    )


# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--sink",
        default="on",
        choices=("on", "off"),
        help="'off' turns quackapi_events off and runs the same assertions — they must fail",
    )
    args = parser.parse_args()

    check_binary()
    prepare_home()

    run_dir = RUNS / time.strftime("%Y%m%dT%H%M%S") / args.sink
    run_dir.mkdir(parents=True, exist_ok=True)
    receiver = run_dir / "receiver.py"
    receiver.write_text(RECEIVER)
    receiver.chmod(0o755)
    sink = run_dir / "requests.jsonl"

    destination = "off" if args.sink == "off" else f"{receiver} {sink}"
    print(f"run directory: {run_dir}")
    print(f"quackapi_events = {destination!r}")

    verdict = [f"quackapi_events = {destination!r}"]

    def report(line: str) -> None:
        verdict.append(line)
        print(line)

    failures = []
    server = Server(destination, FULL_TYPES, run_dir, "main")
    try:
        server.wait_ready()
        for name, fn in (("success", case_success), ("rollback", case_rollback)):
            try:
                fn(server, sink)
                report(f"PASS {name}")
            except Failure as exc:
                failures.append((name, exc))
                report(f"FAIL {name}: {exc}")
    finally:
        server.stop()

    standalone = [
        ("receivers_are_separate_processes", lambda: case_receivers_are_separate_processes(sink)),
        ("narrowed_types", lambda: case_narrowed(run_dir, "off" if args.sink == "off" else str(receiver))),
        ("sink_missing_program", lambda: case_sink_is_down(run_dir, str(run_dir / "no-such-handler"), "missing")),
        ("sink_program_exits_immediately", lambda: case_sink_is_down(run_dir, "/usr/bin/false", "dead")),
        ("gate_refuses_without_events", lambda: case_gate(run_dir, receiver)),
    ]
    for name, fn in standalone:
        try:
            fn()
            report(f"PASS {name}")
        except Failure as exc:
            failures.append((name, exc))
            report(f"FAIL {name}: {exc}")

    report(f"{len(failures)} failed: {', '.join(name for name, _ in failures)}" if failures else "all cases passed")
    (run_dir / "result.txt").write_text("\n".join(verdict) + "\n")
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Failure as exc:
        print(f"FAIL setup: {exc}", file=sys.stderr)
        sys.exit(2)
