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

## Still to test (hypotheses, probes pending)

| # | Edge | Hypothesis | Probe |
|---|------|-----------|-------|
| 2 | SSE / streaming responses | **REAL** — one materialized result, no incremental flush | try chunked write from `respond` in a loop while a query streams rows |
| 3 | WebSockets | **REAL** — stateful, bidirectional, no SQL analog | attempt the upgrade handshake + frame loop in the C UDF |
| 4 | Background tasks (fire-and-forget after response) | **DEFEATED** likely — self-dispatch POST without awaiting / a cron | enqueue work, return response, verify the work runs after |
| 5 | Open transaction across a request (yield-style DI session) | **REAL** likely — dispatch is stateless one-shot | hold a txn across middleware+handler and observe |
| 6 | Dependency injection w/ setup+teardown | **PARTIAL** — params inject fine; `yield`-resource doesn't | model a DB-handle dependency with teardown |
| 7 | Multipart file upload streaming | **PARTIAL** — the C reader buffers; no stream-to-disk in SQL | POST a large multipart body, watch memory |
| 8 | High write throughput / true async | **REAL (bounded)** — single writer serializes | concurrent writers, measure the ceiling |
| 9 | Concurrency of the C listener itself | open — single-thread now | add pthreads to the accept loop; measure |
