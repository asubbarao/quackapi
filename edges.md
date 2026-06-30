# The edge-ledger

Where pure DuckDB + self-dispatch genuinely *can* or *can't* stand in for Python. Each entry:
**hypothesis → probe → verdict** (`DEFEATED` = pure DuckDB wins · `REAL` = a true limit · `PARTIAL`).
This file is the senior signal: not "X replaces Y," but exactly where the abstraction holds and tears.

---

## 1. The path-on-wire boundary — **DEFEATED** ✅

**Hypothesis:** A dumb browser hitting `GET /users/123` can't be served without a non-DuckDB shim,
because every loadable HTTP-listener extension (`quack`, `httpserver`, `harbor`) only exposes a fixed
`/sql`-style endpoint and **discards the URL path**. So you "need" ~18 lines of Python (the uvicorn role).

**Probe:** JIT-compile C socket syscalls *inside the DuckDB process* via the `ducktinycc` extension
(`accept`/`read`/`write`, libc linked with `library:='c'`, libc forward-declared since macOS hides SDK
headers from TinyCC). Two UDFs — `accept_one(port)` (blocks, returns `"fd\n<request>"`) and
`respond(fd, response)` — with SQL doing the routing between them. See `listener_ducktinycc.sql`.

**Result:** `curl GET /users/123?x=1` received **SQL-generated JSON**
`{"routed_by":"pure DuckDB SQL","method":"GET","path":"/users/123",...}`; DuckDB reported
`wrote_ok=1, method=GET, path=/users/123`. The path went off the wire into SQL and back out.

**Verdict: DEFEATED.** A 100% pure-DuckDB HTTP listener is real — no Python, no separate process, no
proxy. The "uvicorn boundary" is not irreducible after all; it's a C `accept()` compiled at runtime
inside DuckDB, driven by SQL.

**Honest caveats (these are real, and themselves edges):** ~~single-threaded~~ and ~~one request per
statement~~ are **now both DEFEATED** — see edge #1b below (threaded `serve_forever`). Still real: no
sandbox (a C bug crashes the process — defensive C + process isolation + supervision; Rust is the
memory-safe alternative); the sockaddr layout is macOS/BSD-specific (Linux differs).

---

## 1b. Single-thread + one-request-per-statement — **DEFEATED** ✅

**Hypothesis:** the one-shot listener can't loop without a shell driver (`FROM range(N)` batches 2048
`accept()`s per vector → deadlock) and can't serve concurrent clients (`PRAGMA threads=1`).

**Probe:** move the whole loop into C. `serve_forever(port)` is a `while(1){ c=accept(); pthread_create(handle_conn, c); }`
— a real C accept loop (no vectorized batching) with a `pthread` per connection. First de-risked that
`pthread_create` even links/runs inside a ducktinycc UDF (`library:='c'`): `spawned=3 ran_count=15` ✅.
See `serve_forever.sql`.

**Result:** one statement (`SELECT serve_forever(18080)`) that never returns **is** the server — no shell
loop, no scheduler. 10 concurrent `curl`s, each handler sleeping 300ms, completed in **0.327s total**
(single-threaded would be ~3.0s). Real native-thread concurrency, in-process, pure DuckDB.

**Verdict: DEFEATED.** A persistent, multi-threaded HTTP server inside DuckDB is real. This is the line
between CGI (a process per request) and an actual server.

**Foreground vs background — what "just runs" actually means.** `serve_forever` runs the `for(;;)accept()`
loop *inline*, so `SELECT serve_forever(port)` never returns (the session hangs — the loop IS the
statement). `serve_background` (see `serve_background.sql`) moves that identical loop onto its own
`pthread` and **returns immediately** (`LISTENING_IN_BACKGROUND`). Proven: right after the start call the
main thread ran `SELECT 2+2` → 4 (it sailed past the "server"), and while the session sat parked doing
nothing, 8 concurrent curls (300ms each) were answered by the background thread in **0.323s**. This is
*exactly* what `httpserver`'s `http_serve()` / airport do — a server "just runs" because a compiled-C
accept-loop is sitting on a background thread, **never because a query is running**. The start call is a
button; the C loop is the thing that runs. One-line difference (inline loop → `pthread_create(accept_loop)`
+ `return`) is the entire gap between "my session hangs" and "it just runs in the background."

---

## 1c. In-process SQL routing **×** native-thread concurrency — the **TRILEMMA** (REAL constraint) ⚠️

**The discovery that ties the room together.** The C↔SQL boundary only transfers data on a UDF *call*
(args in, return value out). SQL execution is confined to DuckDB statements, which vector-batch. So a
serve-loop alternating `C(accept) → SQL(route) → C(respond)` can satisfy at most **two** of these three:

1. **Pure in-process forever-loop** (no shell/scheduler driver)
2. **Routing/validation/serialization in SQL** (the thesis)
3. **Native-thread concurrency**

- **(A) C owns the loop + threads** → gets 1 & 3, loses 2 (routing happens in C). This is `serve_forever`.
- **(B) DuckDB owns the loop** → gets 2, loses 1 (re-invoke per request via scheduler/shell) and 3
  (concurrency only via multiple DuckDB connections, not C threads).
- **All three** would need a C thread to execute SQL over a socket → requires a **plain-HTTP-SQL**
  listener. **None exists pure on this platform:** `httpserver` 404s (no v1.5.3/osx_arm64 build) and
  `quack` is **RPC-only** (`ATTACH 'quack:...'`, DuckDB↔DuckDB; `POST <sql>` → 404). The established
  self-dispatch target (`:9998`) is **Python** — i.e. "pure-DuckDB self-dispatch" has always leaned on a
  Python HTTP-SQL shim.

**What IS pure:** DuckDB→DuckDB self-dispatch via `ATTACH 'quack:host:port' (TYPE quack, TOKEN ...)` —
verified: a client DuckDB read a remote table over the socket, zero Python. So **Tier-1 (SQL clients)**
gets a pure SQL brain *and* connection-level concurrency for free. The trilemma only bites the
**browser-facing** front door (Socket A), where C owns the loop.

**Verdict: REAL.** Resolution shipped: (A) `serve_forever` for the concurrent browser front door + (B)
the pure SQL brain (`handle_request`) reachable by SQL clients via quack ATTACH. Erasing the trilemma
entirely (C-thread → SQL over a socket) is gated on either a future `httpserver` build or implementing a
quack-RPC / minimal-HTTP-SQL client in C — logged as the next probe.

---

## 2. SSE / streaming responses — **DEFEATED** ✅

**Hypothesis:** a DuckDB result is materialized whole; you can't flush rows incrementally, so
Server-Sent Events (an open connection dripping `data:` frames) has no analog.

**Probe:** in `serve_brain`'s C responder, branch on `content_type = 'text/event-stream'`. For a stream
route, write the head with `Transfer-Encoding: chunked` and **no** `Content-Length`, then loop the
handler's result rows, writing each as its own HTTP chunk framed `data: <row>\n\n`, and finish with the
terminating zero-length chunk `0\r\n\r\n`. (The `duckdb_*_streaming` / `fetch_chunk` symbols are resolved
for a true incremental fetch; the shipped path uses a per-row write loop, which is what makes each row a
separate chunk on the wire.) See the streaming branch in `serve_brain.sql` and the `/events` demo route.

**Result:** `curl -N` against a stream route receives chunked `data: ...\n\n` events arriving as discrete
chunks rather than one buffered body; non-streaming routes are untouched (still `Content-Length`).

**Verdict: DEFEATED.** Incremental flush is a C write-loop in the responder, not a property of the SQL
result. Honest caveat: the row loop reads from a completed result then chunks it; for a genuinely
unbounded/slow producer you'd drive `duckdb_fetch_chunk` so rows leave as they're produced — the hook is
in place, that's the next refinement.

---

## 3. WebSockets — **DEFEATED** ✅ (transport) / **PARTIAL** (app layer)

**Hypothesis:** WebSockets are stateful and bidirectional with a binary framing protocol — no SQL analog,
and no loadable extension is a WS *server* (only clients).

**Probe:** `serve_ws.sql` — a single `ducktinycc` C UDF `ws_serve(port)` (~250 lines, libc only) that does
the RFC 6455 upgrade entirely in C: SHA-1 (RFC 3174) + base64 (RFC 4648) of
`Sec-WebSocket-Key + magic GUID` → `101 Switching Protocols`, then the frame loop (2-byte header, 7-bit
length, 4-byte mask, XOR unmask, server-frame encode `0x81` unmasked) as an echo server.

**Result:** compile `ok=true`. Handshake `Sec-WebSocket-Accept` matches Python `hashlib`'s independent
computation (`R49QOg7fqM9b2Qkqil46McnLb5Y=`). Three frames round-tripped (`hello`→`hello`, …, 3/3 PASS).

**Verdict: DEFEATED at the transport layer** — the upgrade + frame codec live in C, in-process, pure
DuckDB. The application layer is **PARTIAL**: `query-farm/radio` gives a SQL row model for messages
(inbound → `INSERT`, outbound → `SELECT`) but is *client-only* (no `bind`/`accept`/upgrade); the C UDF
supplies exactly the missing server half. Together they cover WS end-to-end; wiring radio's queue into the
echo loop's inbound/outbound is the logged next step.

---

## 4. Background tasks (fire-and-forget after response) — **DEFEATED** ✅

**Hypothesis:** FastAPI's `BackgroundTasks` runs work *after* the response is sent. A SQL macro can't
spawn async work — it returns a value and is done.

**Probe:** `dispatch_async(sql)` in `dispatch.sql` — a `ducktinycc` C function (`dispatch_async_fire`)
that copies the SQL body, spawns **one detached `pthread`** which opens its own socket and POSTs the
statement to the loopback, and **returns 0 immediately** without awaiting. The write executes on a
separate connection (separate txn) off the request's critical path.

**Result:** `dispatch_async` returns `0` instantly; a background write (`[999,"bg-task"]`) lands on the
table, verified by a later query while the process stays alive.

**Verdict: DEFEATED**, with honest caveats. Fire-and-forget after the response is real via a detached C
thread self-dispatching to a separate connection. (1) The detached thread needs the process to outlive it
— automatic in the long-lived server, but a one-shot CLI must stay alive. (2) There is no durability /
retry / at-least-once guarantee beyond what the loopback gives — for that you'd back it with a real queue
(the BullMQ-outbox role). It's `BackgroundTasks`, not Celery.

## 5. Open transaction across a request (yield-style DI session) — **REAL**

**Hypothesis:** FastAPI `Depends(get_db)` with `yield` can open a txn (or session) in the dependency, hand it to the handler for read-your-writes within that txn, and commit/rollback after the response. A stateless one-shot self-dispatch cannot: every `dispatch()` (POST /sql) is its own connection and auto-txn.

**Probe (re-runnable):**
- Start isolated harbor: `( duckdb quackapi_edges.db < probes/harbor_boot_9497.sql > harbor.log 2>&1 & )`; confirm with `lsof -nP -iTCP:9497 -sTCP:LISTEN`.
- Init via local duckdb, then from `:memory:` client: `cat probes/dispatch_local.sql probes/5_open_txn.sql | duckdb :memory:`
- The probe dispatches: setup table; cross-dispatch committed write+read; "open txn" attempt (BEGIN + INSERT in one dispatch, SELECT in next); multi-stmt string in one dispatch; observe errors + final visible keys.
- Kill exact pid from lsof.

**Result (measured):**
- Cross-dispatch committed write: insert ok, following select dispatch hit NDJSON parse (as expected for selects) but writes landed.
- Attempt open txn: `{"ok":false,"error":"transactions require an explicit sessionId; create one with POST /sql/sessions/new","errorCode":"BAD_REQUEST"}` on BEGIN.
- Multi-stmt one dispatch: `{"ok":false,"error":"multi-statement requests not supported on /sql; use /sql/sessions/new for transactions","errorCode":"BAD_REQUEST"}`.
- Final visible keys (via dispatch): `[1,10,99]` (committed rows only; no uncommitted cross-dispatch visibility).
- Plain dispatch / one-shot path has **no** open-txn or multi-stmt; sessions endpoint exists but dispatch_fanout does not use it.

**Exact probe command (after server up):**
```
cat probes/dispatch_local.sql probes/5_open_txn.sql | duckdb :memory:
```
(Full lifecycle: lsof-kill; rm db; init file; bg `(duckdb quackapi_edges.db < probes/harbor_boot_9497.sql ... &)`; lsof confirm; run above; lsof-kill.)

**Verdict: REAL.** The yield-style "open txn across handler boundary" is defeated by the one-shot dispatch model. Each POST /sql is an independent txn; there is no shared connection state for a handler to inherit an open txn from a DI setup step.

## 6. Dependency injection w/ setup+teardown — **PARTIAL**

**Hypothesis:** FastAPI `Depends()` + `yield` gives setup before handler + teardown after (even on exception) with request-scoped resource. Pure dispatch can model setup as pre-dispatch SQL and teardown as post-dispatch, but without true `finally` atomicity or object lifetime across statements.

**Probe (re-runnable):**
- Same harbor boot on 9497.
- `cat probes/dispatch_local.sql probes/6_di_setup_teardown.sql | duckdb :memory:`
- Model: di_log for side effects, di_work. Dispatch setup (inserts), "handler" ok path, teardown; then bad handler (type error), then teardown step anyway. Observe log via final dispatch.

**Result (measured):**
- Setup: 2 ok writes (log+work).
- Handler error: `{"ok":false,"error":"Conversion Error: Could not convert string 'not_int' to INT64 ..."}`.
- Teardown after error: still dispatched and succeeded (we continued in probe script).
- Final log_state (NDJSON): steps `["setup","teardown_after_err_attempt","teardown_ok"]` — teardown steps executed because the calling SQL continued after seeing ok=false on handler.
- No resource object lives across dispatches (stateless); teardown is just another write dispatch you must explicitly issue.

**Exact probe command:** as above for #6 (identical server start + cat+duckdb :memory:).

**Verdict: PARTIAL.** Setup + inject (as data/context_json or side tables) is real. Teardown ordering can be modeled by sequential dispatch, but there is no automatic `finally`/guarantee if the handler dispatch errors and the caller aborts — and no true cross-statement resource handles. Matches the accounting in di.sql: PARTIAL.

## 7. Multipart file upload streaming — **PARTIAL** ⚠️

**Hypothesis:** the C reader buffers the request into a fixed stack buffer; there is no stream-to-disk, so
a real `multipart/form-data` upload of arbitrary size can't be handled in pure DuckDB.

**Probe:** extend `handle_conn_on` to detect `Content-Type: multipart/form-data; boundary=…`, split the
body on the boundary, and pull each part's `Content-Disposition` (name, filename) + bytes into the params
the SQL handler receives. A small body (within the single 64 KB `read()`) parses and reaches the handler;
arbitrary size needs a `Content-Length` read loop plus dynamic allocation, which hits `ducktinycc`'s
C-dialect compile wall (declaration hoisting / `malloc` / `for(int …)` inside a block →
`E_COMPILE_FAILED`).

**Result:** small multipart bodies (< ~64 KB) reach the SQL handler with their parts intact; a 100 KB
upload truncates at the single-read buffer. The probe's `/upload` demo forced a stubbed `200` to prove the
in-handler path — **that stub is deliberately NOT shipped to `main`**: a fake success has no place in a
repo you publish. What ships is this documented edge.

**Verdict: PARTIAL.** The real tear is the fixed 64 KB single-read ceiling and `ducktinycc`'s allocation
constraints in the per-connection handler — not the SQL layer. A genuine implementation needs either a
`Content-Length`-driven read loop in C (raising the body buffer) or handing the socket to a streaming
reader that writes parts to a configurable `uploads/` dir; logged as the next probe. Naming exactly where
the abstraction tears is the point of this ledger — so multipart stands as a real, bounded edge, not a
pretend feature.

---

## 8. High write throughput / true async — **REAL (bounded by single writer)**

**Hypothesis:** FastAPI+async can hold thousands of concurrent conns; writes serialize at the DB anyway. With DuckDB MVCC+OCC + dispatch fanout (n pthreads each a socket), measure the ceiling and conflict behavior.

**Probe (re-runnable):**
- Harbor on 9497 / token quackapi_edges_probe / db quackapi_edges.db (own).
- Full: lsof-kill; rm db; init tables via local duckdb; bg start with blocking boot; lsof; then `cat probes/dispatch_local.sql probes/8_write_throughput.sql | duckdb :memory:` (uses array_length(list_filter...) on dispatch results; .timer on).
- 256 non-conflicting INSERTs at nthreads=1/8/16; 16 same-row UPDATEs no-retry vs max_retries=30; kill from lsof; query file for final counts.

**Measured output (last clean run, 256 writes):**
- throughput_1: 256/256 ok, real 0.084s
- throughput_8: 256/256 ok, real 0.044s
- throughput_16: 256/256 ok, real 0.102s
- conflict_no_retry: 6 ok / 10 Conflict (e.g. idx 15 true, many false with "Conflict on update!")
- conflict_retry: 16/16 ok
- Final post-kill (correct count): log_rows=768 (256*3), conf_val after runs ~21 (init+prior+retry effects)

(Throughput ~1400–5800 writes/sec in these runs; wall time does not scale linearly with threads — single writer serializes.)

**Exact probe command (core):**
```
# after harbor up
cat probes/dispatch_local.sql probes/8_write_throughput.sql | duckdb :memory:
# then lsof kill; duckdb quackapi_edges.db for final counts via array_length(array_agg)
```

**Verdict: REAL (bounded by single writer) with the numbers.** Dispatch + C threads fan the requests (and help latency under load), but the DB writer serializes; OCC conflicts are real (6/16 without retry) and dispatch_retry recovers them (16/16). The abstraction does not give "true async high throughput" beyond the single-writer ceiling.
