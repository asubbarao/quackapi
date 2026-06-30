# Self-dispatch — the concurrency engine

A pure-SQL macro cannot `EXECUTE` a runtime string. Self-dispatch is how `handle_request`
runs the handler SQL it renders — and how quackapi gets **true concurrent writes**. Every
mechanism here was chosen only after a simpler/native alternative was **run and measured failing**.

## The split (and why it justifies everything)

The right tool splits by statement kind:

| Statement | Mechanism | Why |
|---|---|---|
| dynamic **SELECT** | native `json_execute_serialized_sql(json_serialize_sql(s))` | in-process, no loopback. PROVEN: runs a dynamic SELECT over a real table. |
| dynamic **write** | loopback self-dispatch to a separate connection | `json_serialize_sql` refuses non-SELECT, and a macro can't `EXECUTE` |
| **concurrent** writes | threaded-C fan-out (`ducktinycc`) | `http_post` over `unnest` does NOT parallelize |
| write-write **conflict** | retry in the C worker (re-POST) | OCC aborts losers; without retry, writes are lost |

The write path's existence is *forced*, not bolted on:

```
SELECT json_serialize_sql('INSERT INTO t SELECT 7');
  -> Parser Error: not implemented: Only SELECT statements can be serialized to json!
```

Only SELECT serializes. A macro can't `EXECUTE`. So a dynamic write must go to another
connection — and the moment it does, you get MVCC + OCC concurrency for free.

## The loopback exec (forced by elimination)

- `httpserver` — no v1.5.3/osx_arm64 build (registry 404).
- `quack` — client speaks Arrow/Flight, not raw HTTP; a C socket can't drive it.
- `harbor` — `POST /sql {"sql":"..."}` over plain HTTP with a Bearer token. **The target.**

```sql
INSTALL harbor FROM community; LOAD harbor;
CALL harbor_serve(bind := '127.0.0.1', port := 9495, token := 'quackapi_loopback');
```

Harbor parallelizes server-side: 8 heavy queries — 1.286 s sequential vs 0.186 s parallel (~8x).

## Why pure SQL can't fan out — measured

| Approach (16 heavy writes) | Wall-time | Verdict |
|---|---|---|
| native `http_post` over `unnest` | 2.552 s | no parallelism (http_client lock; one morsel) |
| threaded-C fan-out, 1 thread | 2.459 s | serial baseline |
| threaded-C fan-out, 8 threads | 0.396 s | ~6.2x |
| threaded-C fan-out, 16 threads | **0.310 s** | **~7.9x, all committed** |

`dispatch_fanout` (a `ducktinycc` UDF) spawns N pthreads, each opening its own socket and
POSTing one statement — OS-thread parallelism the SQL engine can't give over a single morsel.

## Why retry is needed — measured

16 concurrent UPDATEs to the same row, no retry:

```
12/16 -> {"ok":false,"error":"TransactionContext Error: Conflict on update!"}
final value = 7   (9 lost)
```

`dispatch_retry` (worker re-POSTs on "Conflict" with staggered backoff) -> all 16 commit, value 16.

## Public API

```sql
exec_select(sql)                         -- native dynamic SELECT, no loopback
dispatch(sqls, nthreads:=-1,             -- parallel writes; default = machine cores (16),
              max_retries:=0)            --   capped at len(sqls). nthreads:=1 = sequential
dispatch_retry(sqls, max_retries)        -- parallel writes + OCC retry (worker re-POSTs)
dispatch_async(sql)                      -- fire-and-forget; detached C thread
loopback_port()                          -- the ONE place the port literal lives (9495)
```

Design after the design review: in-core JSON throughout (`to_json` builds the envelope,
`from_json` parses the response — the C does zero JSON); threads default to the machine, not 1;
no `list_reduce` fold and no `try_cast(json_extract)` chain (both removed); the port is one
constant (private 9495, distinct from public listener 18099 and quackserver 9494).

## End-to-end (all live)

| Mechanism | Result |
|---|---|
| `exec_select` dynamic read | `[n2, n3, n4]` |
| `dispatch` 50 parallel writes (default threads) | 50/50 ok, all landed, 0.066 s |
| no-retry vs `dispatch_retry` (same-row) | 7 committed -> 16/16 recovered |
| `dispatch_async` | returns 0 instantly; bg write lands `[999,"bg-task"]` |

**Caveat:** fire-and-forget detaches a C thread, so the process must outlive it. Automatic in the
long-lived server; from a one-shot CLI you must keep the process alive.
