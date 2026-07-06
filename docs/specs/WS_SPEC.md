# WebSocket Feasibility Spec — quackapi

**Status:** FEASIBILITY / NOT YET PLANNED  
**Author:** spec agent  
**Date:** 2026-07-02  
**Scope:** Analyzes four implementation options for WebSocket support; recommends one; documents what will not be built.

---

## Preliminary correction: quackapi does NOT use cpp-httplib

The task prompt assumed the HTTP server was cpp-httplib running 16 blocking workers. That is wrong. quackapi's server is entirely its own raw-socket pthread implementation in `ext-cpp/src/quackapi_brain.cpp`. A vendored `ext-cpp/duckdb/third_party/httplib/httplib.hpp` exists in the repository, but it is DuckDB's own copy of cpp-httplib used internally for `httpfs` (the HTTP filesystem client); it is not used by quackapi's accept loop, worker pool, or request handler path. The only reference to "httplib" in `quackapi_brain.cpp` is a comment on line 479 about the DuckDB `SET httpfs_client_implementation='httplib'` setting.

Every claim about "what cpp-httplib can or cannot do" in this spec concerns the vendored header only insofar as it would be relevant if quackapi ever switched to it. For the current server, the constraints come directly from `quackapi_brain.cpp`.

---

## 1. Existing WebSocket work — what `serve_ws.sql` does and its limits

`serve_ws.sql` (root of repo) is a fully working, standalone WebSocket echo server compiled via `ducktinycc` on a separate port (18097). It proves:

- **RFC 6455 handshake is solved.** SHA-1 (RFC 3174 inline, ~50 lines C) + base64 (RFC 4648 inline, ~20 lines C) of `Sec-WebSocket-Key + GUID` → `101 Switching Protocols`. Verified correct 2026-06-29 against Python `hashlib`.
- **Frame codec is solved.** FIN/opcode byte, 7/16/64-bit payload length with two extension cases, 4-byte mask XOR, server-side unmasked text frame (`0x81` + length + payload). Verified round-trip: "hello" → "hello", three frames 3/3 PASS.
- **Per-connection threading is solved.** `pthread_create` + `pthread_detach` per accepted fd, identical to the approach in `quackapi_brain.cpp`'s `accept_loop`.

**Hard limits of the current implementation:**

1. **Separate port (18097), not the main server port.** There is no shared `routes` table, no DI, no auth middleware, no `CREATE ROUTE` DDL. It is an island.
2. **Echo-only.** The frame loop reads a frame and writes it back immediately. There is no SQL dispatch, no per-message handler lookup, no connection identity, no pub/sub.
3. **`ducktinycc` compile surface.** The code runs JIT-compiled inside a DuckDB UDF. The extension C++ server (`quackapi_brain.cpp`) does not include `serve_ws.sql`'s implementation; they are not linked.

**FEATURE_GAP_MATRIX.md row:**
> WebSocket routes: HARD — `serve_ws.sql` separate (RFC6455 in C, echo only), edges.md:3 (transport DEFEATED, app PARTIAL); not mounted on main serve_brain.

`edges.md` verdict: **DEFEATED at transport layer / PARTIAL at app layer.**

---

## 2. The quackapi server model — constraints relevant to WebSockets

From `quackapi_brain.cpp` (verified line numbers):

- **Line 216:** `#define NWORKERS 16` — fixed pool size, no runtime configuration.
- **Lines 503–518:** `accept_loop` — a single background pthread that calls `accept()` and enqueues the fd into `g_q[4096]` (a bounded ring buffer). If `g_qcount >= 4096`, the fd is immediately closed.
- **Lines 491–500:** `worker_main` — 16 persistent pthreads, each with one `duckdb_connect` connection. They block on `pthread_cond_wait` until a fd is available, then call `handle_conn_on(con, fd)`.
- **Lines 221–461:** `handle_conn_on` — reads the request (`read(fd, req, 65535)`), parses method/path/headers/body, runs the C router (`quack_route`), executes handler SQL, writes the response, and calls `close(fd)`. The function always closes the fd before returning.

**The structural consequence for WebSockets:** `handle_conn_on` is a request/response function. It is called once per fd, closes the fd when done, and the worker blocks on the next fd from the queue. A WebSocket connection must hold the fd open for an indefinite frame loop. If a WebSocket connection ran inside `handle_conn_on`, the worker thread would be occupied for the entire connection lifetime — it cannot pick up any other fd until the WS client disconnects.

---

## 3. Options analysis

### Option A: Detect `Upgrade` in the accept loop or worker, then socket hijack

**Concept:** Before routing, detect `Connection: Upgrade` + `Upgrade: websocket` in the raw request. If present, bypass the normal request/response path and hand the fd to a dedicated WS handler that runs the frame loop.

**Structural question: does the current server expose a hook for this?**

No explicit pre-routing hook exists in `quackapi_brain.cpp`. However, `handle_conn_on` reads the full HTTP request into a stack buffer before doing anything else (line 226: `read(fd, req, 65535)`). At that point the function has both the raw request text and the fd. A WS upgrade check could be inserted at line ~266 (after path parsing, before the C router call) — it would read `Connection` and `Upgrade` headers from `headers_json`, branch into a frame loop, and skip `close(fd)` at the end. The fd is in scope and is a plain POSIX file descriptor; there is nothing in the current code preventing this.

**What cpp-httplib offers (for comparison, since it is vendored):** The `Stream` abstract class (`httplib.hpp:872–890`) exposes `virtual socket_t socket() const = 0`, implemented by `SocketStream::socket()` at line 7240 as `return sock_`. A `set_pre_routing_handler(HandlerWithResponse)` hook (`httplib.hpp:1165, 7548`) runs before route dispatch and receives `const Request &` and `Response &` but NOT a `Stream &` — so the fd is not directly accessible in the pre-routing hook without casting through the handler chain. The `HandlerResponse` enum (`httplib.hpp:1116–1118`) has two values: `Handled` and `Unhandled` — returning `Handled` from the pre-routing handler stops processing, but httplib will still call `write_response` to flush the response object. There is no "take ownership of this socket and stop touching it" escape hatch in the public API. Socket hijack via httplib would require subclassing `Server` and overriding `process_and_close_socket` (`httplib.hpp:1285, 8643`), then extracting the fd before httplib's cleanup — this is invasive patching of a vendored file and not applicable to quackapi's actual server.

**In the quackapi C++ server:** Hijack is straightforward because the fd is a plain `int` in scope at the branch point inside `handle_conn_on`. No library abstraction intervenes. A WS frame loop function (already proven in `serve_ws.sql`) could be called with the same fd, and the function would simply not call `close(fd)` at the end of the normal path.

**The worker pinning problem:** This is the real constraint. Each WS connection holds one of 16 workers for its lifetime. With N concurrent WS connections, N workers are unavailable to HTTP. With N=16, the HTTP server deadlocks: `accept_loop` enqueues fds but all 16 workers are in WS frame loops. The fd queue fills to 4096 and then new connections are dropped.

**Verdict on Option A:** Technically feasible in the current codebase. The hard limit is the worker pool.

---

### Option B: Dedicated WS thread per accepted WS connection (bypassing the worker pool)

**Concept:** Extend `accept_loop` (or add a second accept path) so that when a WS `Upgrade` request is detected, it does NOT enqueue the fd into `g_q`. Instead, it immediately spawns a dedicated `pthread` (detached) that runs the WS frame loop directly, independent of the 16 HTTP workers.

**Why this works:** The WS frame loop in `serve_ws.sql` already uses `pthread_create` + `pthread_detach` per connection (lines 306–311 of `serve_ws.sql`). `quackapi_brain.cpp` already uses `pthread_create` + `pthread_detach` in `accept_loop` itself. Combining them: when `accept_loop` sees an `Upgrade` request, it peeks at the first few bytes of the HTTP request (a non-blocking `recv` with `MSG_PEEK`), detects the upgrade, and passes the fd to a WS-specific thread rather than the HTTP queue.

**The detection problem:** `accept_loop` currently calls `accept()` and immediately enqueues the fd without reading anything. To detect WS upgrades, `accept_loop` would need to do a non-blocking peek (`recv(fd, buf, N, MSG_PEEK | MSG_DONTWAIT)`) and then branch. This adds latency and complexity to the accept path for every connection. Alternatively, the HTTP worker can do the detection after reading (Option A style), then — instead of closing the fd — spawn a WS thread and return without closing.

**Worker pool isolation:** Under this model, WS connections run on their own threads (N dedicated threads for N WS clients), completely separate from the 16 HTTP workers. HTTP throughput is unaffected. The only shared resource is the DuckDB instance if WS handlers execute SQL (each WS thread needs its own `duckdb_connect`).

**Thread count:** N concurrent WS clients = N threads. This is unbounded. A cap (`WS_MAX_CONNECTIONS`) with graceful rejection on overflow is required to avoid fd exhaustion. The `serve_ws.sql` echo server uses `g_args[1024]` slots (line 240) — 1024 is a reasonable starting cap.

**Verdict on Option B:** This is the cleanest architectural fit. HTTP workers are unaffected. WS threads are isolated. The RFC 6455 implementation already exists and can be lifted directly from `serve_ws.sql`.

---

### Option C: Keep the separate-port design as the shipped approach

**Concept:** Accept that WS and HTTP live on different ports. Document that `ws_serve(18097)` is a parallel server, not integrated with the HTTP routes registry, middleware, or auth. The SQL surface for WS is separate and explicit.

**FastAPI comparison:** FastAPI mounts `@app.websocket("/ws")` on the same app object as HTTP routes. It shares DI, lifespan, and middleware. A separate-port WS in quackapi does not match this and cannot claim "same app" behavior.

**What it gives:** Zero risk, zero C++ changes. The echo server already works today. A SQL app can run both `SELECT serve_brain(9494, db)` and `SELECT ws_serve(18097)` in parallel (two DuckDB statements on two connections, or two processes, or two background pthreads).

**What it does not give:**
- Shared auth: the WS server has no access to the `routes`, `middleware`, or `param_schema` tables unless it opens its own DuckDB connection to the same file DB.
- Shared routing DDL: `CREATE ROUTE ... WS` would need new parser support.
- Unified observability: two ports, two servers, no cross-server request tracing.

**Verdict on Option C:** Honest and working today. Not a FastAPI-equivalent, which the FEATURE_GAP_MATRIX already documents as a known gap. Acceptable for the "pure track" claim (reference implementation, not supplant). Shipping Option C and calling it a separate server is consistent with the edges ledger's honesty standard.

---

### Option D: Replace the server with one that natively supports HTTP Upgrade

**Candidates:** uWebSockets (C++, epoll/kqueue, WS-native), Boost.Beast (header-only, WS over Boost.Asio), or libev/libuv with manual WS codec.

**Cost:** High. quackapi's server is ~300 lines of tight C in `quackapi_brain.cpp` with zero external dependencies beyond the C standard library and POSIX pthreads. Replacing it with Boost.Asio or uWebSockets means:
- Adding a large dependency (Boost.Beast: ~1 MB headers; uWebSockets: requires libuv/libssl).
- Rewriting the accept loop, worker model, keep-alive logic, SSE chunked streaming, and the `g_rt` integration.
- Losing the DuckDB C-API integration pattern (`resolve_sym`, per-worker `duckdb_connect`) that the current design is built around.
- Increasing build complexity for a DuckDB extension, which must compile against DuckDB's own vendored headers.

**What it buys:** First-class WS + HTTP coexistence at the library level, with event-driven accept (not pthread-per-WS-connection). For high WS concurrency (thousands of connections), epoll-based servers scale better than one-thread-per-connection.

**Verdict on Option D:** Effort L. Unjustified for the current project scope. The one-thread-per-WS-connection model (Option B) serves hundreds of concurrent WS connections adequately; thousands of simultaneous persistent WS connections are not a stated quackapi use case, and the current user base is one developer.

---

## 4. Recommendation

**Recommended: Option B — per-connection WS thread, bypassing the HTTP worker pool.**

**Rationale:**

- The RFC 6455 codec is already written and verified (`serve_ws.sql`). Copy the C implementation into `quackapi_brain.cpp` directly — no new dependency, no new compilation unit.
- HTTP workers are completely isolated. Zero impact on HTTP throughput.
- `CREATE ROUTE ... WS` can be added as a new kind in `routes.kind` (joining `dynamic`, `stream`, `static`, `openapi`, `html`, `redirect`). The C router (`quack_route`) detects `kind='ws'` and the worker thread that detects the WS upgrade reads the matched route to find the per-message handler SQL.
- The separate-port `serve_ws.sql` becomes a test artifact / teaching example, not the production path.

**Option C (separate port) is acceptable as interim shipping state** if Option B is deferred. It is already working today. Document it as "WS transport proven in isolation; integration is a future milestone."

---

## 5. SQL surface design for Option B

### 5.1 Route registration

A WS route is registered like any other route, with `kind='ws'`:

```sql
-- Declarative (existing INSERT path):
INSERT INTO routes (route_id, method, pattern, handler, kind, status)
VALUES ('ws_chat', 'GET', '/ws/chat', 'SELECT ... FROM messages WHERE ...', 'ws', 101);

-- DDL syntax (would require parser extension):
CREATE ROUTE ws_chat GET '/ws/chat' AS SELECT to_json(m) FROM messages m ORDER BY id DESC LIMIT 1;
-- The parser would need a new KIND clause or auto-infer from the method token 'WS'.
```

The cleaner DDL form would use a `WS` pseudo-method (since WS connections begin as `GET` with an `Upgrade` header):

```sql
CREATE ROUTE ws_chat WS '/ws/chat' AS <handler_sql>;
```

The C parser extension (`RouteDdlParse` in `quackapi_extension.cpp`, lines 167–313) already tokenizes method as a string (`to_upper(meth)`). Adding `WS` as a recognized method token requires one additional branch — the stored `method` would be `WS`, and the router matches `WS` routes only when the incoming request has `Upgrade: websocket`.

### 5.2 Handler model — per-message request/response (recommended)

**This is the only honest handler model for a stateless SQL engine.**

Each received WS text frame is treated as a unit of input. The handler SQL is rendered with a `{message}` token substituted with the frame payload (same mechanism as `{id}`, `{q}` in HTTP path/query params). The result rows are serialized to JSON and sent back as one or more WS text frames.

```
client frame  →  {message} substituted in handler SQL  →  duckdb_query  →  rows  →  server frames
```

Example:

```sql
CREATE ROUTE ws_echo WS '/ws/echo'
  AS SELECT '{"echo": ' || to_json({message}) || '}';
```

```sql
CREATE ROUTE ws_query WS '/ws/query'
  AS SELECT to_json(u) FROM users u WHERE u.name ILIKE {message};
```

The C WS worker thread (one per connection) runs this loop:

```
while true:
  frame = ws_read_frame(fd)
  if close frame: break
  render handler_sql with frame payload
  result = duckdb_query(con, rendered_sql)
  for each row: ws_write_text(fd, row)
```

The handler executes in a per-connection DuckDB connection (`duckdb_connect` called once at WS thread start). Each frame invocation is a separate transaction (same model as HTTP workers). There is no open transaction across frames.

### 5.3 What this gives

- Per-message query/response: the SQL engine answers each client frame with a query result. Works for: query over WS, echo, lookup, live-poll-on-send.
- Multiple result rows per frame: each row becomes a separate WS text frame.
- Auth via route matching: the `ws_chat` route can require a header param (`kind=header`) that the C router validates before entering the frame loop. If auth fails → 401 and close, never enters WS mode.
- Shared `routes` registry: same DDL, same reload, same parity oracle (`handle_request` can detect `kind='ws'` and return a synthetic 101 body for testing the match logic).

### 5.4 What this does NOT give

- **Server push.** The handler only fires when a client frame arrives. There is no way for the server to push a frame spontaneously without a client message triggering it. A long-poll pattern (`SELECT ... WHERE id > {last_seen}`) can approximate this, but requires the client to send a poll frame.
- **Shared connection state across frames.** Each frame invocation is a fresh `duckdb_query`. There is no cursor, no open transaction, no session variable that persists between frames (same REAL edge as #5 in `edges.md` — yield-style DI cannot hold an open txn across SQL dispatches).
- **Broadcast / pub-sub.** Sending a message to all connected WS clients requires a shared data structure that all WS threads can write/read. DuckDB does not provide this; it would require a C-level concurrent queue (a new global in `quackapi_brain.cpp`, similar to `g_q[4096]` but for WS messages). UNVERIFIED whether this is feasible without a global lock that serializes all WS sends.
- **Binary frames.** The `serve_ws.sql` codec handles only text frames (opcode 1) and close frames (opcode 8). Binary frames (opcode 2) would require an additional code path.
- **Subprotocols.** No `Sec-WebSocket-Protocol` negotiation. The server sends `101` without selecting a subprotocol.
- **Ping/Pong.** The echo server does not implement opcode 9 (ping) or opcode 10 (pong). RFC 6455 §5.5 requires responding to ping frames with pong.

---

## 6. FastAPI comparison

| Capability | FastAPI / Starlette | quackapi (Option B shipped) |
|---|---|---|
| WS endpoint on same port as HTTP | Yes — `@app.websocket("/ws")`, same ASGI app | Yes — same `routes` registry, same port, `kind='ws'` |
| Shared DI / middleware | Yes — `Depends()`, lifespan, middleware stack runs | Partial — auth via param validation before WS upgrade; full middleware chain does NOT run on WS (no `pre_routing_handler` equivalent in the WS code path) |
| `await websocket.receive_text()` loop | Yes — async coroutine, can `await` across frames | No — synchronous blocking `read()` per frame on a dedicated pthread; no async runtime |
| Server push (send without receive) | Yes — `await websocket.send_text(msg)` at any time | No — only per-frame handler response |
| Broadcast / pub-sub | Via external state (Redis, etc.) or shared dict | Not built; would require a new C-level broadcast queue |
| Subprotocols | `websocket.accept(subprotocol=...)` | Not implemented |
| Ping/Pong keepalive | Yes — Starlette handles automatically | Not implemented |
| Binary frames | Yes | Not implemented |
| JSON model on receive | Pydantic parsing inside handler | SQL template substitution (`{message}`) |

**Honest summary:** quackapi Option B covers the core use case of a query-on-message WebSocket endpoint. It does not cover server push, pub-sub, or keep-alive primitives. For a "chat app" (client sends a message, server looks it up in the DB, replies) it works. For a "live feed" (server pushes new rows to all connected clients as they arrive) it does not, and claiming otherwise would be false.

---

## 7. Concurrency model under Option B

**Setup:** 16 HTTP workers (pthreads, persistent DuckDB connections). N WS connections = N additional threads (detached pthreads, each with its own DuckDB connection).

**Thread count math:**
- 16 HTTP workers always running.
- Each WS client: +1 thread, +1 DuckDB connection.
- At 100 concurrent WS clients: 116 threads, 116 DuckDB connections to one DuckDB instance.
- DuckDB connection overhead per connection is a full `duckdb_connect()` plus the one-time `SET threads=1 / LOAD shellfs / LOAD curl_httpfs` sequence (copied from `worker_main`). Connection setup cost for WS is paid once per connection, not per frame.

**DuckDB concurrency under WS load:**
The `edges_round6_draft.md` (edge #10, verified numbers) shows 16 concurrent workers on the same DuckDB instance achieve approximately 4.8× the serial throughput, not 16×. The bottleneck is per-query `ThreadContext` + `OperatorProfiler` setup under shared `DatabaseInstance` state (profiler from `PROFILE_SEARCH.md`). Adding 100 WS connections, each running a SQL query per received frame, would add 100 more concurrent `duckdb_query` callers to the same instance. The scaling ceiling does not increase linearly. At high WS + HTTP combined load, latency would rise on both channels.

**Proposed mitigation:**
- **WS connection cap:** `WS_MAX_CONNECTIONS` (suggested default: 64 or 128). On accept, if the WS thread count is at cap, send `HTTP/1.1 503 Service Unavailable` and close. No WS frame loop is entered.
- **Per-WS DuckDB connection pool:** Not implemented. Each WS thread opens its own `duckdb_connect`. This is the same model as HTTP workers.
- **No dedicated WS DuckDB instance:** Running a separate DuckDB `duckdb_open` for WS connections (eliminating shared `DatabaseInstance` contention) would give WS threads their own query scheduling budget, but they would only see data committed to the on-disk file, not in-memory state. Acceptable for read-heavy WS workloads; not workable for write-then-read within a session.

**HTTP worker pool isolation:** Under Option B, WS connections never occupy HTTP workers. A sustained WS client with a slow frame rate does not affect HTTP throughput. This is the critical property of Option B vs Option A.

---

## 8. Effort estimate

| Option | Effort | Blocking issue |
|---|---|---|
| A (upgrade hijack inside HTTP worker) | S | Worker starvation — 16 WS clients = no HTTP |
| B (per-connection WS thread, bypass workers) | M | None blocking; WS cap + per-thread DuckDB connection setup needed |
| C (separate port, keep as-is) | S (already done) | No integration with HTTP routes/auth/DI |
| D (replace server with uWebSockets/Beast) | L | Full server rewrite, large dependency |

**Option B breakdown:**
- Port `ws_serve.sql`'s handshake + frame codec into `quackapi_brain.cpp` as C functions (not inline ducktinycc): ~200 lines already proven, translates directly. **S**.
- Add `kind='ws'` detection to `quack_route` and `handle_conn_on` — branch before normal response path: ~30 lines. **S**.
- WS thread spawn with capped counter: ~50 lines. **S**.
- `CREATE ROUTE ... WS` parser extension: add `WS` as a recognized method token in `RouteDdlParse` (`quackapi_extension.cpp:174`): ~10 lines change. **S**.
- Per-frame SQL dispatch loop (lifted from HTTP worker pattern): ~60 lines. **S**.
- Tests (`.test.sql` parity oracle — `handle_request` returns `101` for a `WS` route match; `ws_serve` round-trip test via `serve_ws.sql` adapted): **S**.

Total: **M** (medium, 2–4 days of focused work, single developer).

---

## 9. What quackapi will NOT build under this spec

1. **Server push.** No spontaneous frame sends. SQL is stateless-query-oriented; a push model requires a C-level event queue that changes the fundamental server design.
2. **Pub-sub / broadcast.** No mechanism to fan out a message from one WS connection to all others. Would require shared mutable state with concurrent access from N WS threads — a non-trivial lock-free data structure or a queue that adds a new subsystem.
3. **Ping/Pong keepalive frames** (RFC 6455 §5.5 opcode 9/10). These are separate opcodes from data frames and require active handling to prevent clients from timing out. DEFERRED (not in scope for initial WS milestone).
4. **Binary frames** (opcode 2). Only text frames (opcode 1) are handled. Binary payloads require an alternate `ws_write_binary` path.
5. **Subprotocol negotiation** (`Sec-WebSocket-Protocol`). Server accepts any upgrade without subprotocol selection.
6. **WS over TLS.** TLS is already a gap for HTTP (FEATURE_GAP_MATRIX §12). WS over TLS (wss://) inherits this gap entirely.
7. **Shared transaction across frames.** Each frame is a fresh `duckdb_query` in its own auto-commit transaction. No `BEGIN ... COMMIT` across two frames.
8. **More than ~128 concurrent WS clients** (at the cap). This is a design-appropriate limit for an embedded single-process server; it is not a bug.

---

## 10. Implementation notes — socket hijack in the current server

The handshake detection in `handle_conn_on` (`quackapi_brain.cpp:221`) should happen after `headers_json` is built (~line 373) and before the `g_rt` router call (~line 376). The check:

```c
// after headers_json is populated
char *upgrade = quack_json_extract_string(headers_json, "upgrade");
bool is_ws = (upgrade && strcasecmp(upgrade, "websocket") == 0);
if (upgrade) free(upgrade);

if (is_ws) {
    // do NOT close(fd) here; the WS thread owns it
    ws_handle_upgrade(fd, g_rt, req, path, headers_json);
    return;  // worker returns immediately; fd lifecycle is WS thread's
}
```

`ws_handle_upgrade` would: validate the WS route exists in `g_rt`, extract `Sec-WebSocket-Key`, run the handshake (101), spawn a detached pthread with a `ws_conn_arg` struct containing `{fd, con, route_def*}`, and return. The spawned thread owns the DuckDB connection for its lifetime.

Note: `handle_conn_on` receives `void *con` (the worker's persistent DuckDB connection). The WS thread must NOT use this connection — it would race with the next HTTP request on the same worker. The WS thread must call `g_ddb_connect(g_db, &ws_con)` to open its own connection.

---

## Executive Summary

**Recommended option:** Option B — per-connection WS thread, bypassing the HTTP worker pool. Uses the proven RFC 6455 code from `serve_ws.sql` (SHA-1 + base64 handshake, 2–14 byte frame codec, verified working), lifted into `quackapi_brain.cpp`. WS connections spawn dedicated detached pthreads (capped at a configurable `WS_MAX_CONNECTIONS`, suggested 64–128). HTTP workers are never occupied by WS connections — the pools are orthogonal. A new `kind='ws'` in the `routes` table enables `CREATE ROUTE ws_name WS '/path' AS <sql>` via a small parser extension change. Per-message handler model: each received text frame substitutes into the handler SQL template and returns row(s) as WS frames — stateless, fits the SQL engine, honest.

**Effort:** Medium (2–4 days). No new dependencies. RFC 6455 implementation is already written and verified.

**One hard limit:** Server push is architecturally out of scope. quackapi can answer a WS frame with a SQL query result; it cannot spontaneously push data to connected clients without a C-level broadcast queue that does not exist and is not planned. This is the honest gap vs. FastAPI's `await websocket.send_text(msg)` from a background task. For query-on-message patterns (real-time lookup, live search, DB-backed echo) quackapi matches FastAPI. For push-based feeds it does not.
