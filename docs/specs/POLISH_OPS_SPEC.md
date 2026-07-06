# POLISH_OPS_SPEC — quackapi Polish + Production-Ops Wave

**Status:** Implementation contract. All behavior in this document must be reproduced exactly.  
**Date:** 2026-07-02  
**Scope:** HEAD auto-handling, OPTIONS / 405 semantics, gzip compression, graceful shutdown, access logs.  
**Non-scope:** TLS termination, multipart upload, WebSocket integration (separate tracks per FEATURE_GAP_MATRIX.md).

Citations legend:
- `[V]` = verified against source code via web fetch during spec authoring (URL + line noted).
- `[U]` = unverified; from general HTTP-RFC knowledge or extrapolation from verified neighbors. Flag for re-check before merge.

---

## Layer ownership boundary

This is the single most important architectural statement in this spec.

| Concern | SQL oracle (`handle_request`) | C layer only (`quackapi_brain.cpp`) |
|---------|------------------------------|-------------------------------------|
| HEAD route matching | YES — oracle must match HEAD | YES — C router must match HEAD |
| HEAD body suppression | NO — oracle returns body as usual | YES — C write path zeroes body bytes |
| 405 Allow header | YES — oracle returns Allow in resp_headers | YES — C router builds Allow and sets resp_headers |
| OPTIONS 204 preflight (CORS) | YES — already in middleware pre-phase (R1 CORS) | YES — C must run apply_pre before route match |
| OPTIONS non-CORS 405 | YES — oracle produces 405 + Allow | YES — C router mirrors |
| gzip compression | NO — oracle contract is always uncompressed | YES — C write path only |
| graceful shutdown | NO — SQL has no process lifecycle | YES — C accept_loop + worker_main |
| access logs | NO | YES — C write path only |

---

## 1. Automatic HEAD

### 1.1 What Starlette does

Source: `starlette/routing.py` lines 430–432 [V]:

```python
if "GET" in self.methods:
    self.methods.add("HEAD")
```

HEAD is added to the allowed-methods set at route registration time. The route then matches HEAD requests as it matches GET — same handler is called, full response body is computed.

Body suppression source: `uvicorn/protocols/http/h11_impl.py` line 493 [V]:

```python
data = b"" if self.scope["method"] == "HEAD" else body
```

The ASGI server (uvicorn) zeroes the body bytes before writing to the socket. The ASGI application (Starlette/FastAPI) never knows the body was discarded. The `Content-Length` header is **not recomputed** — it remains the length of the would-be GET body. This is correct per RFC 9110 §9.3.2: HEAD response MUST send the same header fields as GET but MUST NOT send a message body.

### 1.2 quackapi required behavior

**SQL oracle (`handle_request`):**

| Input method | Route registered | Expected oracle output |
|---|---|---|
| `HEAD` | GET route exists for that path | Same as `GET`: status=route.status, content_type, body (full body string), handler_sql if dynamic, resp_headers |
| `HEAD` | No route for that path | 404 `{"detail":"Not Found"}` |
| `HEAD` | Path exists but only POST registered | 405 (see §2) |

The oracle returns the full body string for HEAD — it is C's job to suppress it on the wire.

**Implementation steps for SQL oracle:**

The `matched` CTE currently filters `ri.method = method`. HEAD must either:

Option A (preferred): In the `matched` CTE, treat HEAD as GET for the purpose of route lookup:

```sql
WHERE (ri.method = method OR (method = 'HEAD' AND ri.method = 'GET'))
```

Option B: Register a HEAD route alias at boot time (less clean; do not use).

Use Option A. The rest of the oracle pipeline (param extraction, validation, handler render, resp_headers) is identical to GET.

**C layer (quackapi_brain.cpp):**

In `handle_conn_on` and `quack_route`, the method string arrives from the request line. After routing produces a `RouteDecision`:

```c
// HEAD body suppression — in the write path, after dec.body or hbody is computed
if (strcmp(method, "HEAD") == 0) {
    // write headers with Content-Length = original length, but body bytes = 0
    int bl = dec.body ? (int)strlen(dec.body) : 0;
    // OR for dynamic routes: bl = hbody ? (int)strlen(hbody) : 0;
    char hdr[1024];
    int hl = snprintf(hdr, 1024, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                      dec.status, reason, ctype, bl);
    write(fd, hdr, hl);
    // DO NOT write body bytes
    close(fd);
    return;
}
```

The C router must also add HEAD to the matched-method set when looking up routes: when the incoming method is HEAD, attempt match against GET routes. This mirrors the SQL oracle Option A.

### 1.3 HEAD parity test cases (tier2_http.sh style)

```bash
# H1 — HEAD on a GET route: 200, correct Content-Length, empty body
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X HEAD "$BASE_URL/health")
check "HEAD /health → 200" "$([ "$STATUS" = "200" ] && echo 1 || echo 0)" "status=$STATUS"

HEADERS=$(curl -s -I -X HEAD "$BASE_URL/health")
CL=$(echo "$HEADERS" | grep -i '^content-length:' | tr -d ' \r\n' | cut -d: -f2)
BODY_BYTES=$(curl -s -X HEAD "$BASE_URL/health" | wc -c | tr -d ' ')
check "HEAD /health → Content-Length present" "$([ -n "$CL" ] && echo 1 || echo 0)" "headers=$HEADERS"
check "HEAD /health → zero body bytes on wire" "$([ "$BODY_BYTES" = "0" ] && echo 1 || echo 0)" "bytes=$BODY_BYTES"

# H2 — HEAD Content-Length matches GET Content-Length
GET_BODY=$(curl -s "$BASE_URL/health")
GET_LEN=${#GET_BODY}
HEAD_CL=$(curl -s -I -X HEAD "$BASE_URL/health" | grep -i '^content-length:' | tr -d ' \r\n' | cut -d: -f2)
check "HEAD /health → Content-Length = GET body length" "$([ "$HEAD_CL" = "$GET_LEN" ] && echo 1 || echo 0)" "head_cl=$HEAD_CL get_len=$GET_LEN"

# H3 — HEAD on dynamic route: 200, no body
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X HEAD "$BASE_URL/users/1")
check "HEAD /users/1 → 200" "$([ "$STATUS" = "200" ] && echo 1 || echo 0)" "status=$STATUS"

# H4 — HEAD on missing path: 404
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X HEAD "$BASE_URL/nonexistent")
check "HEAD /nonexistent → 404" "$([ "$STATUS" = "404" ] && echo 1 || echo 0)" "status=$STATUS"

# H5 — HEAD on POST-only path: 405 (see §2)
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X HEAD "$BASE_URL/users")
# /users has both GET and POST, so HEAD → 200. Use a POST-only route for true 405 test.
```

**Oracle-level test (pure SQL, no server):**

```sql
-- H-oracle-1: HEAD on static route returns same body as GET
SELECT
  (SELECT body FROM handle_request('HEAD', '/health', '{}', '')) =
  (SELECT body FROM handle_request('GET',  '/health', '{}', '')) AS bodies_match;
-- expected: true

-- H-oracle-2: HEAD on dynamic route returns handler_sql (body=NULL)
SELECT status_code, body IS NULL AS no_body, handler_sql IS NOT NULL AS has_hsql
FROM handle_request('HEAD', '/users/1', '{}', '');
-- expected: 200, true, true
```

---

## 2. Automatic OPTIONS and 405 Semantics

### 2.1 What Starlette does

Source: `starlette/routing.py` lines 430–432, 544–551 [V].

**Key findings verified:**

1. HEAD is auto-added when GET is registered (§1.1 above). Therefore the Allow header for a GET-only route is `GET, HEAD`.

2. OPTIONS is **not** auto-handled by Starlette's core routing. An OPTIONS request on a path that has only GET registered returns **405 Method Not Allowed** with `Allow: GET, HEAD`. Starlette does not auto-generate a 200 OPTIONS response [V].

3. The 405 response uses `PlainTextResponse("Method Not Allowed", status_code=405)` with the `Allow` header. The body text is `Method Not Allowed` (plain text), not JSON. FastAPI overrides this with its exception handler to return JSON — but that is a FastAPI-layer concern, not Starlette routing.

4. CORS OPTIONS (preflight) is handled by Starlette's `CORSMiddleware` as a pre-phase intercept, not by routing. quackapi already implements this in `middleware.sql` CORS pre-phase (R1 spec §7) [V via R1_REQUEST_SURFACE_SPEC.md].

**Allow header computation rule:**

For a path `/p`:
- Collect all `method` values from `routes` WHERE `pattern = '/p'` (or the matched pattern).
- If `GET` is in that set, add `HEAD`.
- Sort methods for determinism (alphabetical order is conventional but not RFC-required).
- Join with `, `.

Example: route table has GET `/users` and POST `/users` → Allow: `GET, HEAD, POST`.

### 2.2 quackapi required behavior

**SQL oracle (`handle_request`) changes:**

The oracle currently does `WHERE ri.method = method` in `matched`. For OPTIONS and 405, the oracle needs two new behaviors:

**Behavior A — 405 with Allow header:**

When path segments match at least one route but the method does not match any route for that path, return:

```
status_code: 405
content_type: application/json
body: {"detail": "Method Not Allowed"}
handler_sql: NULL
resp_headers: {"Allow": "<computed allow string>"}
```

Note: quackapi uses JSON bodies for all error responses (FastAPI layer behavior, not plain Starlette). Body is `{"detail": "Method Not Allowed"}`.

**Behavior B — OPTIONS (non-CORS):**

When method is OPTIONS and CORS middleware pre-phase did NOT short-circuit (no CORS config, or non-preflight OPTIONS), return the same 405 shape with the Allow header. Do not auto-generate 200 for OPTIONS. This matches Starlette routing behavior [V].

**Implementation in `handle_request`:**

Add a new CTE `path_matches` that finds all routes matching the path segments regardless of method:

```sql
path_matches AS (
  SELECT ri.route_id, ri.method
  FROM route_idx ri, req r
  WHERE ri.seg_count = len(r.req_segs)
    AND len(list_filter(
          list_zip(r.req_segs, ri.pat_segs),
          lambda p: NOT (starts_with(p[2], '{') OR p[1] = p[2])
        )) = 0
),
allow_methods AS (
  SELECT
    CASE WHEN list_contains(list(pm.method), 'GET')
         THEN list_sort(list_distinct(list_concat(list(pm.method), ['HEAD'])))
         ELSE list_sort(list(pm.method))
    END AS methods,
    array_length(list(pm.method)) > 0 AS path_exists
  FROM path_matches pm
),
```

Then in the final SELECT, after the `route_id IS NULL` (404) check, add a 405 branch:

```sql
CASE
  WHEN route_id IS NULL AND (SELECT path_exists FROM allow_methods) THEN 405
  WHEN route_id IS NULL THEN 404
  ...
END AS status_code,
```

And in body:

```sql
WHEN route_id IS NULL AND (SELECT path_exists FROM allow_methods)
  THEN cast(json_object('detail', 'Method Not Allowed') AS VARCHAR)
WHEN route_id IS NULL THEN cast(json_object('detail', 'Not Found') AS VARCHAR)
```

And in resp_headers for 405:

```sql
WHEN route_id IS NULL AND (SELECT path_exists FROM allow_methods)
  THEN json_object('Allow', array_to_string((SELECT methods FROM allow_methods), ', '))
```

HEAD must also be included in `path_matches` derivation: when computing the Allow header, if GET is in `path_matches.method`, add HEAD to the set.

**C layer (quackapi_brain.cpp):**

In `quack_route`, after the match loop exits with `best < 0`:

```c
if (best < 0) {
    /* Check if path matches any route regardless of method */
    char allow_buf[256]; allow_buf[0] = 0;
    bool has_get = false;
    bool path_hit = false;
    for (int i = 0; i < rt->count; i++) {
        const RouteDef *rd = &rt->routes[i];
        if (rd->seg_count != nreq) continue;
        bool m = true;
        for (int j = 0; j < nreq; j++) {
            if (rd->pat_segs[j][0] == '{') continue;
            if (strcmp(rd->pat_segs[j], req_segs[j]) != 0) { m = false; break; }
        }
        if (!m) continue;
        path_hit = true;
        /* collect method */
        if (allow_buf[0]) strncat(allow_buf, ", ", sizeof(allow_buf)-strlen(allow_buf)-1);
        strncat(allow_buf, rd->method, sizeof(allow_buf)-strlen(allow_buf)-1);
        if (strcmp(rd->method, "GET") == 0) has_get = true;
    }
    if (path_hit) {
        if (has_get) {
            strncat(allow_buf, ", HEAD", sizeof(allow_buf)-strlen(allow_buf)-1);
        }
        char rh[512];
        snprintf(rh, sizeof(rh), "{\"Allow\":\"%s\"}", allow_buf);
        RouteDecision d = {405, strdup("application/json"),
                           strdup("{\"detail\":\"Method Not Allowed\"}"),
                           NULL, false, strdup(rh)};
        quack_free_segments(req_segs, nreq);
        quack_free_query_params(qarr, nq);
        return d;
    }
    /* true 404 */
    quack_free_segments(req_segs, nreq);
    quack_free_query_params(qarr, nq);
    return defdec;
}
```

Note: the Allow header method order in the C path is accumulation order (route table scan order), not alphabetical. If determinism is required, sort the allowed set before building the string.

### 2.3 Exact input → output table

| Request | Routes table state | Expected status | Expected body | Expected Allow header |
|---|---|---|---|---|
| `DELETE /users` | GET + POST on `/users` | 405 | `{"detail":"Method Not Allowed"}` | `GET, HEAD, POST` (sorted) |
| `OPTIONS /users` | GET + POST on `/users`, no CORS config | 405 | `{"detail":"Method Not Allowed"}` | `GET, HEAD, POST` |
| `OPTIONS /users` | GET + POST on `/users`, CORS middleware active with preflight headers | 204 | `` (empty) | *(set by CORS pre-phase, not routing)* |
| `DELETE /nope` | No routes for `/nope` | 404 | `{"detail":"Not Found"}` | *(none)* |
| `HEAD /health` | GET on `/health` | 200 | *(body on wire: 0 bytes; full body string in oracle)* | *(none in resp_headers; N/A)* |
| `HEAD /users` | GET + POST on `/users` | 200 | *(wire: 0 bytes)* | N/A |
| `HEAD /nope` | None | 404 | *(wire: 0 bytes)* | N/A |

### 2.4 Parity test cases

```bash
# O1 — DELETE on GET+POST route: 405 + Allow
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$BASE_URL/users")
check "DELETE /users → 405" "$([ "$STATUS" = "405" ] && echo 1 || echo 0)" "status=$STATUS"
HDRS=$(curl -s -I -X DELETE "$BASE_URL/users" 2>&1)
ALLOW=$(echo "$HDRS" | grep -i '^allow:' | head -1)
check "DELETE /users → Allow header present" "$(echo "$ALLOW" | grep -qi 'GET' && echo 1 || echo 0)" "allow=$ALLOW"
check "DELETE /users → Allow includes HEAD" "$(echo "$ALLOW" | grep -qi 'HEAD' && echo 1 || echo 0)" "allow=$ALLOW"

# O2 — DELETE on nonexistent path: 404 (not 405)
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$BASE_URL/nonexistent")
check "DELETE /nonexistent → 404" "$([ "$STATUS" = "404" ] && echo 1 || echo 0)" "status=$STATUS"

# O3 — OPTIONS on GET+POST route (no CORS): 405 + Allow
STATUS=$(curl -s -o /dev/null -w '%{http_code}' -X OPTIONS "$BASE_URL/users")
check "OPTIONS /users (no CORS) → 405" "$([ "$STATUS" = "405" ] && echo 1 || echo 0)" "status=$STATUS"

# O4 — 405 body is JSON
BODY=$(curl -s -X DELETE "$BASE_URL/users")
HIT=$(dq "SELECT json_extract_string('$(sql_esc "$BODY")', '$.detail') AS v;")
check "405 body → JSON detail field" "$([ "$HIT" = "Method Not Allowed" ] && echo 1 || echo 0)" "body=$BODY"
```

**Oracle-level test (pure SQL):**

```sql
-- O-oracle-1: DELETE /users → 405 with Allow in resp_headers
SELECT status_code,
       json_extract_string(resp_headers, '$.Allow') AS allow_hdr
FROM handle_request('DELETE', '/users', '{}', '');
-- expected: status_code=405, allow_hdr contains 'GET' and 'POST' and 'HEAD'

-- O-oracle-2: DELETE /nonexistent → 404 (no Allow)
SELECT status_code, body
FROM handle_request('DELETE', '/nonexistent', '{}', '');
-- expected: 404, '{"detail":"Not Found"}'

-- O-oracle-3: OPTIONS /users (no CORS) → 405
SELECT status_code FROM handle_request('OPTIONS', '/users', '{}', '');
-- expected: 405

-- O-oracle-4: HEAD /health → same status as GET
SELECT
  (SELECT status_code FROM handle_request('HEAD', '/health', '{}', '')) =
  (SELECT status_code FROM handle_request('GET',  '/health', '{}', '')) AS status_match;
-- expected: true
```

---

## 3. gzip Compression

### 3.1 What Starlette's GZipMiddleware does

Source: `starlette/middleware/gzip.py` verified [V]:

| Parameter | Default | Notes |
|---|---|---|
| `minimum_size` | **500 bytes** | Responses shorter than this are NOT compressed |
| `compresslevel` | **9** | zlib levels 1–9; 9 = best compression, highest CPU |
| SSE exclusion | Yes — `DEFAULT_EXCLUDED_CONTENT_TYPES = ("text/event-stream",)` | SSE streams are never compressed |
| Trigger | `"gzip" in request.Accept-Encoding` | No Accept-Encoding gzip → no compression (Identity responder used) |
| Response header set | `Content-Encoding: gzip` | Added when compression applied |
| Vary header | `Vary: Accept-Encoding` | Added when compression applied (line 101/111) |
| Content-Length | Recomputed to compressed size for buffered responses | Deleted for streaming responses (line 85) |
| Double-compress guard | Yes — if `Content-Encoding` already set on response, skips compression | Checked at line 34 |

**What gzip middleware does NOT do:**

- Does not compress when `Content-Encoding` is already set on the response.
- Does not compress `text/event-stream` content.
- Does not compress responses below `minimum_size` (unless the response is a streaming response with `more_body=True` — in that case, buffering continues regardless of size).
- Does not alter any other headers beyond `Content-Encoding`, `Vary`, and `Content-Length`.

### 3.2 Layer ownership for quackapi

**The SQL oracle (`handle_request`) contract is always uncompressed.** This boundary is non-negotiable:

- `handle_request` returns the raw body string. gzip is transport-level packaging, not application semantics.
- Test cases in Tier-1 (pure SQL) and Tier-2 (HTTP without Accept-Encoding: gzip) run against uncompressed bodies.
- The oracle parity contract (`parity_b2.sh` style) remains uncompressed.

**gzip lives exclusively in the C write path**, applied after the RouteDecision body is computed (for static and dynamic routes) or after each SSE chunk is emitted (SSE is excluded entirely).

### 3.3 C-layer implementation contract

Add a `bool g_gzip_enabled` global (default: false; enabled by a `gzip` parameter on `serve_brain` or `serve_brain_sql`). When enabled:

```
COMPRESSION DECISION:
  compress = false
  if g_gzip_enabled:
    if request has Accept-Encoding header containing "gzip":
      if content_type is NOT "text/event-stream":
        if response body length >= 500:
          compress = true

IF compress:
  compressed_body = zlib_deflate(body, level=9, format=gzip)
  response headers:
    Content-Encoding: gzip
    Content-Length: len(compressed_body)
    Vary: Accept-Encoding
  write compressed_body
ELSE:
  response headers as current (Content-Length: len(body))
  write body as-is
```

**Header matrix (exact):**

| Condition | Content-Encoding | Content-Length | Vary |
|---|---|---|---|
| gzip disabled | *(absent)* | `len(body)` | *(absent)* |
| gzip enabled, no `Accept-Encoding: gzip` in request | *(absent)* | `len(body)` | *(absent)* |
| gzip enabled, gzip accepted, body ≥ 500 bytes, not SSE | `gzip` | `len(compressed)` | `Accept-Encoding` |
| gzip enabled, gzip accepted, body < 500 bytes | *(absent)* | `len(body)` | *(absent)* |
| gzip enabled, gzip accepted, SSE route | *(absent)* | *(chunked, no change)* | *(absent)* |
| gzip enabled, response already has Content-Encoding set | *(not added)* | *(unchanged)* | *(absent)* |

**zlib call:**

```c
#include <zlib.h>
// compress2 or deflateInit2 with windowBits=31 (gzip format)
z_stream strm = {0};
deflateInit2(&strm, Z_BEST_COMPRESSION /*level 9*/, Z_DEFLATED,
             31 /* windowBits=15+16 for gzip wrapper */,
             8 /* memLevel */, Z_DEFAULT_STRATEGY);
```

### 3.4 Performance constraint

**gzip MUST be OFF by default.** When off, it adds zero work on the hot path — no header inspection, no branch beyond checking `g_gzip_enabled`. A simple `if (!g_gzip_enabled) goto write_plain;` at the top of the compression block is sufficient. The project benchmark (edges.md) shows ~41k r/s for static routes; a 3% regression (~1.2k r/s drop) is a rejection threshold for this feature.

### 3.5 gzip parity test cases

```bash
# G1 — no Accept-Encoding: no compression
BODY=$(curl -s "$BASE_URL/users")
ENC=$(curl -s -I "$BASE_URL/users" | grep -i '^content-encoding:')
check "GET /users no gzip → no Content-Encoding" "$([ -z "$ENC" ] && echo 1 || echo 0)" "enc=$ENC"

# G2 — with Accept-Encoding: gzip, large response: compressed
BODY=$(curl -s -H 'Accept-Encoding: gzip' --compressed "$BASE_URL/users")
ENC=$(curl -s -I -H 'Accept-Encoding: gzip' "$BASE_URL/users" | grep -i '^content-encoding:')
check "GET /users with gzip → Content-Encoding: gzip (if body>=500)" \
      "$(echo "$ENC" | grep -qi 'gzip' && echo 1 || echo 0)" "enc=$ENC"
# Note: /users body may be < 500 bytes with default 3-user dataset; use a route with larger body for reliable G2.

# G3 — with gzip accepted, small body: no compression
# (requires a route with body < 500 bytes; /health body is ~16 bytes)
ENC=$(curl -s -I -H 'Accept-Encoding: gzip' "$BASE_URL/health" | grep -i '^content-encoding:')
check "GET /health with gzip (small body) → no Content-Encoding" "$([ -z "$ENC" ] && echo 1 || echo 0)" "enc=$ENC"

# G4 — SSE route with gzip: no compression, chunked only
ENC=$(curl -s -I -H 'Accept-Encoding: gzip' "$BASE_URL/events" | grep -i '^content-encoding:')
check "GET /events with gzip → no Content-Encoding (SSE excluded)" "$([ -z "$ENC" ] && echo 1 || echo 0)" "enc=$ENC"

# G5 — Vary: Accept-Encoding present when gzip applied
VARY=$(curl -s -I -H 'Accept-Encoding: gzip' "$BASE_URL/<large-route>" | grep -i '^vary:')
check "Compressed response has Vary: Accept-Encoding" "$(echo "$VARY" | grep -qi 'Accept-Encoding' && echo 1 || echo 0)" "vary=$VARY"

# G6 — decompressed body matches uncompressed body
RAW=$(curl -s "$BASE_URL/users")
GZIP_DECOMPRESSED=$(curl -s -H 'Accept-Encoding: gzip' --compressed "$BASE_URL/users")
check "Decompressed gzip body = raw body" "$([ "$RAW" = "$GZIP_DECOMPRESSED" ] && echo 1 || echo 0)" "diff"
```

---

## 4. Graceful Shutdown

### 4.1 What uvicorn does

Source: `uvicorn/server.py` [V]:

**Signal handling:**

| Signal | First occurrence | Second SIGINT |
|---|---|---|
| SIGTERM | `should_exit = True` | — |
| SIGINT (Ctrl+C) | `should_exit = True` | `force_exit = True` (immediate) |

**Shutdown sequence (uvicorn/server.py lines 289–331) [V]:**

1. Stop accepting new connections (close server socket).
2. Close passed sockets (fd-passing setups).
3. Call `connection.shutdown()` on each active ASGI connection (signals in-flight handlers to wrap up).
4. Drain: wait for in-flight connections to finish. Each wait-loop iteration checks `force_exit`; a second SIGINT aborts the drain.
5. Drain timeout: `timeout_graceful_shutdown` (config param, default `None` meaning no timeout). If timeout elapses with connections still open, cancel remaining tasks.
6. Trigger lifespan shutdown event (skipped if `force_exit`).

`timeout_graceful_shutdown` default is `None` [V — uvicorn/main.py line 413–416]. `None` means drain waits indefinitely for in-flight requests to complete naturally before force-closing.

### 4.2 quackapi model: 16 blocking-worker pthreads

**There is no vendored httplib in this codebase.** The spec prompt's reference to "cpp-httplib" was incorrect — quackapi's server is a hand-rolled TCP accept loop in `quackapi_brain.cpp` using raw POSIX sockets (`socket/bind/listen/accept/read/write`), 16 `pthread` workers consuming from a bounded queue `g_q[4096]`, and a detached `accept_loop` thread. There is no `cpp-httplib` or vendored `httplib.h` header in `ext-cpp/src/include/` (only `quackapi_extension.hpp` is present). All shutdown behavior must be designed against the actual brain.cpp primitives.

**What "graceful shutdown" means for this model:**

| uvicorn concept | brain.cpp equivalent |
|---|---|
| Stop accepting | `close(g_listen)` or set a `g_shutdown` flag checked in `accept_loop` |
| Drain in-flight | Wait for in-flight `handle_conn_on` calls to return (workers finish current fd) |
| Timeout | `pthread_cond_timedwait` or a watchdog thread |
| Force-close | `pthread_cancel` on worker threads (dangerous; alternative: `close(fd)` on queued fds) |
| DB detach | `g_ddb_disconnect(&con)` on each worker, then `g_ddb_close(&g_db)` |

**Required implementation in brain.cpp:**

```c
// Globals
static volatile int g_shutdown_requested = 0;
static volatile int g_inflight = 0;          // atomic counter
static pthread_mutex_t g_inflight_mu;
static pthread_cond_t  g_inflight_cv;
static int g_shutdown_timeout_s = 30;        // default 30s; 0 = infinite

// In accept_loop:
void *accept_loop(void *arg) {
    for (;;) {
        int c = accept(g_listen, 0, 0);
        if (c < 0) {
            if (g_shutdown_requested) break;   // EINTR after close(g_listen)
            continue;
        }
        if (g_shutdown_requested) { close(c); break; }  // drain: reject new
        // ... existing queue push ...
    }
    return 0;
}

// At start of handle_conn_on:
pthread_mutex_lock(&g_inflight_mu);
g_inflight++;
pthread_mutex_unlock(&g_inflight_mu);

// At end of handle_conn_on (before return):
pthread_mutex_lock(&g_inflight_mu);
g_inflight--;
pthread_cond_signal(&g_inflight_cv);
pthread_mutex_unlock(&g_inflight_mu);

// Signal handler:
static void handle_shutdown(int sig) {
    g_shutdown_requested = 1;
    close(g_listen);   // unblocks accept() with EBADF/EINTR
    g_listen = -1;
}

// Drain function (called from a cleanup thread or from block_forever_impl):
void drain_and_close(void) {
    // Drain queued-but-not-started fds
    pthread_mutex_lock(&g_qm);
    while (g_qcount > 0) {
        int fd = g_q[g_qhead]; g_qhead = (g_qhead+1)%4096; g_qcount--;
        close(fd);  // reject queued connections
    }
    pthread_mutex_unlock(&g_qm);

    // Wait for in-flight to reach 0
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += g_shutdown_timeout_s > 0 ? g_shutdown_timeout_s : 86400;
    pthread_mutex_lock(&g_inflight_mu);
    while (g_inflight > 0) {
        int rc = pthread_cond_timedwait(&g_inflight_cv, &g_inflight_mu, &deadline);
        if (rc == ETIMEDOUT) break;  // force-close path
    }
    pthread_mutex_unlock(&g_inflight_mu);

    // Disconnect worker DuckDB connections (workers will EINVAL on next query)
    // NOTE: worker cons are thread-local; they disconnect when worker thread exits
    // Clean: close g_db after workers are done
    if (g_db) { g_ddb_close(&g_db); g_db = 0; }
}
```

**Signal registration in `serve_brain_impl`:**

```c
signal(SIGTERM, handle_shutdown);
signal(SIGINT,  handle_shutdown);
signal(SIGPIPE, SIG_IGN);  // already present
```

**Shutdown timeout default:** 30 seconds (quackapi-specific choice; uvicorn defaults to None/infinite, but infinite is impractical for a C process with no async cancellation). Expose as a parameter on `serve_brain` / `serve_brain_sql`.

### 4.3 "Stop accepting vs drain in-flight" semantic

Workers are blocking: a worker thread calling `g_ddb_query(con, hsql, &res)` cannot be interrupted by `close(g_listen)`. The drain window is:

- Connections **in the queue** (`g_q`, waiting for a worker): close their fds immediately (reject).
- Connections **being handled** (inside `handle_conn_on`): wait for `write(fd, ...)` + `close(fd)` to complete naturally.
- DuckDB queries in progress: these complete (cannot be interrupted mid-query); the drain timeout is the backstop.

Closing `g_listen` unblocks `accept()` with `EBADF`. Workers reading from `g_q` continue normally until the queue empties.

### 4.4 Graceful shutdown test cases

These are observable via the live server only (no oracle equivalent):

```bash
# S1 — SIGTERM drains and exits cleanly
curl -s "$BASE_URL/users/slow" &    # triggers 300ms handler (slow knob in brain.cpp:266)
PID=$(pgrep -f "duckdb.*launch_server")
sleep 0.05   # in-flight window
kill -SIGTERM "$PID"
wait "$PID"
check "SIGTERM → clean exit" "$([ $? -eq 0 ] && echo 1 || echo 0)" "exit=$?"

# S2 — No new connections accepted after SIGTERM
kill -SIGTERM "$PID"
sleep 0.1
STATUS=$(curl -s -o /dev/null -w '%{http_code}' --connect-timeout 1 "$BASE_URL/health" 2>/dev/null || echo "000")
check "Post-SIGTERM → connection refused" "$([ "$STATUS" = "000" ] && echo 1 || echo 0)" "status=$STATUS"

# S3 — DB detached: no stale lock files
check "No WAL lock files after shutdown" "$([ ! -f quackapi.db.wal ] && echo 1 || echo 0)" ""
```

---

## 5. Access Logs

### 5.1 What uvicorn does

Source: `uvicorn/protocols/http/h11_impl.py` line 478–486 [V]:

```python
self.access_logger.info(
    '%s - "%s %s HTTP/%s" %d',
    get_client_addr(self.scope),
    self.scope["method"],
    get_path_with_query_string(self.scope),
    self.scope["http_version"],
    status,
)
```

Exact format produced: `127.0.0.1:54321 - "GET /users HTTP/1.1" 200`

Fields in order: `remote_addr:port - "METHOD path?query HTTP/version" status_code`

uvicorn access logging is **always on** when `--access-log` is set (the default), and emitted per-request via Python's logging framework. There is no per-request duration in the default uvicorn format [U — not seen in h11_impl.py; some third-party formatters add it].

### 5.2 quackapi required behavior

**Access logs are OFF by default.** Enabling is via a flag parameter to `serve_brain` / `serve_brain_sql` (or an environment variable `QUACKAPI_ACCESS_LOG=1`). When OFF, the hot path does zero additional work — no timestamp, no snprintf, no write call. A single `if (!g_access_log)` guard before the logging block is the entire cost.

**Format (one line per request, to stderr):**

```
<remote_addr> - "<METHOD> <path_with_query> HTTP/1.1" <status> <duration_ms>ms
```

quackapi adds `<duration_ms>` which uvicorn's default format omits. This is the one intentional extension for ops utility. Remote addr is `"<IP>:<port>"` from `getpeername(fd)`, or `"-"` if unavailable.

**Exact example:**

```
127.0.0.1:54321 - "GET /users HTTP/1.1" 200 1ms
```

**Implementation:**

```c
// At top of handle_conn_on (after g_access_log check):
struct timespec t_start;
if (g_access_log) clock_gettime(CLOCK_MONOTONIC, &t_start);

// After write(fd, ...) and before close(fd):
if (g_access_log) {
    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    long ms = (long)((t_end.tv_sec - t_start.tv_sec) * 1000)
            + (long)((t_end.tv_nsec - t_start.tv_nsec) / 1000000);
    char peer[64] = "-";
    struct sockaddr_in paddr; socklen_t paddrlen = sizeof(paddr);
    if (getpeername(fd, (struct sockaddr*)&paddr, &paddrlen) == 0) {
        snprintf(peer, sizeof(peer), "%s:%d",
                 inet_ntoa(paddr.sin_addr), ntohs(paddr.sin_port));
    }
    fprintf(stderr, "%s - \"%s %s HTTP/1.1\" %d %ldms\n",
            peer, method, path, final_status, ms);
    fflush(stderr);
}
```

**Performance constraint:** This is the project's hardest constraint for this feature. When `g_access_log == 0`, the hot path executes zero extra instructions. `clock_gettime`, `getpeername`, `snprintf`, and `fprintf` are only called when the flag is on. The flag is a `volatile int` set once at startup; no per-request locking.

The benchmark ceiling for `/health` (static, zero DuckDB) is ~41k r/s. Access logging when enabled is expected to reduce throughput by ~10–15% (two `clock_gettime` calls + one `getpeername` + one `fprintf(stderr)` per request). This is acceptable **only when explicitly enabled**. When disabled, the overhead is zero.

### 5.3 Access log test cases

```bash
# L1 — no log output when disabled (default)
SERVER_STDERR=$(mktemp)
duckdb < launch_server.sql 2>"$SERVER_STDERR" &
sleep 0.5
curl -s "$BASE_URL/health" > /dev/null
curl -s "$BASE_URL/users" > /dev/null
sleep 0.1
LOG_LINES=$(wc -l < "$SERVER_STDERR" | tr -d ' ')
check "Access log OFF → zero log lines from requests" "$([ "$LOG_LINES" = "0" ] && echo 1 || echo 0)" "lines=$LOG_LINES"

# L2 — log line present when enabled
# (requires launching server with access_log=true parameter)
LOGLINE=$(grep 'GET /health HTTP/1.1' "$SERVER_STDERR" | head -1)
check "Access log ON → line contains method+path" "$([ -n "$LOGLINE" ] && echo 1 || echo 0)" "line=$LOGLINE"

# L3 — log line format: remote_addr - "METHOD path HTTP/1.1" status ms
check "Log line format matches pattern" \
  "$(echo "$LOGLINE" | grep -qE '^[0-9.]+ - "GET /health HTTP/1.1" 200 [0-9]+ms$' && echo 1 || echo 0)" \
  "line=$LOGLINE"

# L4 — status code in log matches HTTP status
ERRLINE=$(grep 'GET /nonexistent' "$SERVER_STDERR" | head -1)
check "404 in access log" "$(echo "$ERRLINE" | grep -q ' 404 ' && echo 1 || echo 0)" "line=$ERRLINE"
```

---

## 6. Implementation Order and Effort Ranking

| Rank | Feature | Layer | Effort | Risk | Prerequisite |
|---|---|---|---|---|---|
| 1 | HEAD route matching (SQL oracle) | SQL | Low | None | None |
| 2 | 405 with Allow header (SQL oracle) | SQL | Medium | Medium (CTE additive complexity) | Needs path_matches CTE |
| 3 | HEAD body suppression (C write path) | C | Low | Low | HEAD matching in C router |
| 4 | 405 Allow in C router (quack_route) | C | Low | Low | None |
| 5 | Graceful shutdown SIGTERM/SIGINT | C | Medium | High (race between drain and queue) | None |
| 6 | Access logs (C write path, flag-gated) | C | Low | Low | None |
| 7 | gzip (C write path, flag-gated, zlib) | C | Medium | Low | zlib linkage in CMakeLists |

---

## 7. Surprises and Unverified Items

### Verified surprises

1. **Starlette does NOT auto-handle OPTIONS [V].** Common assumption is that web frameworks auto-generate OPTIONS responses. Starlette returns 405 for OPTIONS if no OPTIONS route is registered. CORS preflights work only because `CORSMiddleware` intercepts before routing. quackapi's CORS middleware already mirrors this correctly (R1 spec §7).

2. **HEAD body suppression happens at the ASGI SERVER level, not Starlette routing [V].** The uvicorn `h11_impl.py` zeroes the body; Starlette's routing layer runs the GET handler in full and returns the complete body. Implication: quackapi's oracle is correct to return the full body for HEAD — the C write path suppresses it.

3. **uvicorn's `timeout_graceful_shutdown` defaults to `None` [V].** This means infinite wait unless a signal arrives. For a blocking C process without async cancellation, infinite drain is impractical; quackapi should default to 30s.

4. **uvicorn's access log does NOT include request duration [V].** The default format is `addr - "METHOD path HTTP/version" status`. quackapi's addition of `<duration_ms>ms` is a deliberate extension.

5. **gzip compresslevel default is 9 (maximum compression) [V].** This is CPU-expensive. For a latency-sensitive server, level 6 is often a better tradeoff (comparable compression ratio, ~50% faster). The spec follows Starlette's default (9) for parity; consider making configurable.

6. **SSE (`text/event-stream`) is explicitly excluded from gzip in Starlette [V].** quackapi must mirror this: the `/events` stream route is never compressed even when gzip is enabled and the client sends `Accept-Encoding: gzip`.

### Unverified items (marked [U])

- The exact Allow header method sort order (alphabetical vs registration order) is not specified in the HTTP RFC or Starlette source. The Starlette code does `", ".join(self.methods)` where `self.methods` is a `set` — Python set ordering is non-deterministic. [U] Alphabetical sort is recommended for quackapi for deterministic tests.

- uvicorn's access log format for requests where path contains a query string: the format uses `get_path_with_query_string(scope)` which includes `?q=foo` in the log line. Assumed quackapi mirrors this (path+query in one field). [U — not verified against a live uvicorn instance; based on code reading only.]

- Whether uvicorn sends a second SIGTERM to trigger `force_exit` (vs SIGINT): the source shows second SIGINT → force_exit, but first SIGTERM only sets `should_exit`. A second SIGTERM does NOT trigger `force_exit` per the code at line 341–348. [V — code read; behavior: SIGTERM → graceful, SIGTERM+SIGTERM → still graceful, SIGINT+SIGINT → force].

---

## 8. What This Spec Does Not Cover

- TLS termination (HARD gap, separate track).
- WebSocket upgrade detection in the same accept loop (separate spec).
- Access log to file vs stderr rotation (deferred; stderr is sufficient for ops).
- Prometheus / structured metrics endpoint.
- gzip for streaming SSE responses (excluded by Starlette default; kept excluded here).
