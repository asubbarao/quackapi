# Events — a request's lifecycle, reported outside the process

quackapi answers every HTTP request on a DuckDB connection of its own and
destroys it when the request ends. That connection's query and that query's
transaction **are** the request. The [`events`](https://duckdb.org/community_extensions/extensions/events.html)
community extension reports both to a program quackapi never links: one JSON
object per event, written to that program's stdin.

**Why bother, given `quackapi_otlp`.** Spans are emitted from inside the
process that is being observed. `transaction_rollback` is the event you want
precisely when that process is the thing that went wrong, and a request that
fails is told nothing but `500` — the account of *why* has to leave the
process to be worth anything.

## What this is not

These are **query and transaction lifecycle events**. There is no row event,
no table event and no CDC in this extension, and nothing here is a step
towards one. If you want to know which rows changed, this is the wrong tool.

The full set the extension can emit: `connection_opened`, `connection_closed`,
`query_begin`, `query_end`, `transaction_begin`, `transaction_commit`,
`transaction_rollback`, `planning_error`, `finalize_prepare`,
`execute_prepared`, `rebind_prepared_statement`.

## Delivery guarantee, stated plainly

| Mode | What happens | What is guaranteed |
|------|--------------|--------------------|
| `quackapi_events_async = false` (default) | DuckDB forks, writes the JSON to the handler's stdin, closes it, and **waits for the handler to exit**. The exit code is logged. | The handler ran before the query returned. Nothing is retried, nothing is buffered: if the handler drops the bytes, the event is gone and the query still succeeds. |
| `quackapi_events_async = true` | Fire and forget. | **Nothing.** Not ordering, not arrival, not that the handler ever started. You buy latency with the entire guarantee. |

Either way the request outranks its telemetry: a missing handler program, or
one that exits without reading stdin, costs the event and **not** the request
— see [When the sink is down](#when-the-sink-is-down).

A new process is spawned **per event**. At the default event list that is five
`fork`+`exec` pairs per request, plus two for the request-id stamp below. This
is an observability tool for a service where that is affordable, or a
short-lived handler that hands off to something durable.

## Settings

| Setting | Type | Default | Meaning |
|---------|------|---------|---------|
| `quackapi_events` | VARCHAR | `'off'` | Handler command line — a program and its arguments. `'off'` leaves the `events` settings untouched. |
| `quackapi_events_types` | VARCHAR[] | `['query_begin','query_end','transaction_begin','transaction_commit','transaction_rollback']` | Event types to deliver. |
| `quackapi_events_async` | BOOLEAN | `false` | Fire-and-forget (see above). |

quackapi moves `events_destination`, `events_types` and `events_async`
**globally**. It has to: a plain `SET` is session-local, and a request runs on
a connection the caller never touched, so a session-local destination would
reach nothing quackapi serves.

`connection_opened` is deliberately not in the default list. `events` fires it
before DuckDB has assigned the connection id, so it arrives as
`18446744073709551615` and correlates with nothing.

## The gate

A configured handler is a promise `quackapi_serve` keeps or refuses:

```
Invalid Configuration Error: quackapi_events requires the 'events' extension:
… Install it with INSTALL events FROM community, then retry.
```

quackapi `LOAD`s the companion and never `INSTALL`s it. A download inside
serve turns an offline box, or a renamed community package, into a server that
quietly records nothing.

## Correlating a request to its events

Three keys, all of which both ends can name:

| Key | Where the client gets it | Where the event carries it |
|-----|--------------------------|----------------------------|
| request id | the `X-Request-ID` response header (minted, or the one the client sent) | `session_name` on **every** event of that request |
| connection id | a route that selects it from `quackapi_events()` | `connection_id` |
| transaction id | the same route | `transaction_id` on the query and transaction events |

quackapi names the request's connection with the request id before the handler
runs, so the events of a request that **fails** — whose body says nothing —
still carry it. That stamp is one `SET` statement on the request's own
connection, so it appears in the sink as its own `query_begin`/`query_end`
pair. That is the price of the id being on everything after it.

```sql
SET quackapi_events = '/bin/sh -c "cat >> /tmp/quackapi-events.jsonl"';
SELECT destination, types, async, state, detail FROM quackapi_events();

CREATE ROUTE obs GET '/obs' AS
SELECT connection_id, transaction_id, session_name FROM quackapi_events();
```

`quackapi_events()` both applies the settings and reports them, so
`SET` then `SELECT` is how you move the sink after `LOAD`. Its `state` is
`off`, `serving` or `unavailable`, and `detail` says which.

## Reading the sink back

The handler's output is JSONL, one object per line, and it is the raw layer:
nothing rebuilds it. Read it whole with a reader and select the named columns
the extension emits — do not pull fields out of the text with a selector.

```sql
CREATE VIEW raw_events AS
SELECT * FROM read_json('/tmp/quackapi-events.jsonl',
                        format := 'newline_delimited', sample_size := -1);

SELECT session_name,
       connection_id,
       transaction_id,
       list_sort(array_agg(event)) AS events,
       len(events) AS n,
       array_agg(DISTINCT has_error) AS error_flags
FROM raw_events
GROUP BY session_name, connection_id, transaction_id;
```

`sample_size := -1` reads every record before fixing the schema. The default
decides off a prefix that `has_error` does not exist and then drops it from
every later row.

### What comes out

Three requests against the routes in `examples/events.sql` — one `GET /obs`,
one `POST /add` that commits, one `POST /add` that violates the primary key:

```
│   session_name   │ connection_id │ transaction_id │  events                                                                      │ n │ error_flags         │
│ example-obs      │ 15            │ 0              │ [query_end, query_end, query_end, query_end]                                 │ 4 │ [false]             │
│ example-obs      │ 15            │ 22             │ [transaction_commit]                                                         │ 1 │ [NULL]              │
│ example-obs      │ 15            │ 23             │ [transaction_begin, transaction_commit]                                      │ 2 │ [NULL]              │
│ example-obs      │ 15            │ 24             │ [query_begin, transaction_begin, transaction_commit]                         │ 3 │ [NULL]              │
│ example-rollback │ 17            │ 0              │ [query_end, query_end, query_end, query_end]                                 │ 4 │ [false, true]       │
│ example-rollback │ 17            │ 32             │ [query_begin, transaction_begin, transaction_rollback, transaction_rollback] │ 4 │ [true, NULL, false] │
```

Three things in there are worth knowing before you build on it, and none of
them are quackapi's doing:

- **`query_end` always lands under `transaction_id` 0.** It fires after the
  transaction has closed, and `events` reads the id at that moment, so the id
  is already gone. `query_begin` carries the real one. Group the `query_end`
  rows separately; the transaction id a route reports matches its
  `query_begin`, `transaction_begin` and `transaction_commit`/`rollback`.
- **`transaction_rollback` arrives twice for one failed statement**, once with
  `has_error: true` and once with `false`.
- **The error itself is only in the sink.** `error_message` on the failing
  `query_end` and rollback carries `Constraint Error: Duplicate key "id: 1"
  violates primary key constraint.` — the client got `{"detail":"Internal
  Server Error"}` and nothing else. This is the whole reason to run it.

A request also produces more `query_end` events than it has statements of its
own: the request-id stamp is one, and quackapi's per-request query deadline is
another.

**Order in the file is arrival order, not event order.** Each event is written
by its own process; with `async = true` several are in flight at once. Sort by
`timestamp` if you need a sequence, and note it has millisecond resolution.

## When the sink is down

| Situation | Result |
|-----------|--------|
| `events_destination` names a program that does not exist | `exec` fails in the child, the child exits 127, the parent logs it. The request is answered normally. |
| The handler exits immediately without reading stdin | The write to the closed pipe fails, the parent logs the exit code. The request is answered normally. |
| The `events` extension is not installed | `quackapi_serve` refuses to start and names the install command. It does not serve blind. |

A broken sink costs events. It does not cost requests — `test/integration/test_events.py`
asserts exactly that, against a handler path that does not exist and against
`/usr/bin/false`.

## Proving it

```bash
python3 test/integration/test_events.py            # the sink on
python3 test/integration/test_events.py --sink off # the same assertions, sink off: they must fail
```

The second form is part of the suite. It starts the same server with
`quackapi_events = 'off'` and runs the same correlations, which then have
nothing to correlate: `success`, `rollback`, `narrowed_types` and
`receivers_are_separate_processes` all fail. The three cases that are *about*
an absent or broken sink keep their own destinations and still pass. A test
whose subject can be removed without the test noticing has proved nothing.

See [`examples/events.sql`](../../examples/events.sql) for the whole flow in
one session, including a rolled-back request.
