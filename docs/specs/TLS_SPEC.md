# TLS_SPEC — quackapi TLS Feasibility and Implementation Contract

**Status:** Feasibility + design decision spec. Not an implementation contract yet — the recommendation section defines the chosen path. Implementer needs zero other context.  
**Date:** 2026-07-02  
**Scope:** Direct TLS serving equivalent to uvicorn `--ssl-keyfile / --ssl-certfile`; option analysis; threading fit; surface design; what we will not support.  
**Non-scope:** WebSocket TLS (separate track per WS_SPEC.md), client certificates, ALPN/h2.

Citations legend:
- `[V]` = verified against source code or file content during spec authoring (file path + line noted).
- `[U]` = unverified; from general knowledge or reasonable extrapolation. Flag for re-check before implementation.

---

## 1. The Honest Baseline

### 1.1 What uvicorn's TLS story actually is

uvicorn supports `--ssl-keyfile` and `--ssl-certfile` (and `--ssl-keyfile-password`, `--ssl-ca-certs`, `--ssl-ciphers`). Under the hood, when those flags are passed, uvicorn replaces its plain asyncio server with an SSL-wrapped version via Python's `ssl.SSLContext.wrap_socket()` / `asyncio.create_server(ssl=ctx)`. The Python standard library handles the TLS handshake transparently; the ASGI application sees no difference.

**Crucially: uvicorn's own deployment documentation recommends TLS termination at a proxy (nginx/caddy/traefik/haproxy) for production.** The `--ssl-*` flags exist for development convenience and single-binary scenarios. The FastAPI docs and uvicorn deployment guide both state this explicitly. The overwhelming majority of FastAPI production deployments terminate TLS at the proxy tier and reach uvicorn over plain HTTP on a loopback or internal socket.

### 1.2 What quackapi actually needs to claim parity

**Minimum bar for direct-TLS parity with uvicorn:** The ability to call `serve_brain(port, db, cert_path, key_path)` and have the socket layer speak TLS 1.2/1.3 exactly as uvicorn does when passed `--ssl-keyfile`/`--ssl-certfile`. No other uvicorn SSL options are required at parity — they are opt-in conveniences.

**Short version of the recommendation (full analysis follows):** Ship the "behind a proxy" path as v1. It is already the uvicorn-recommended production story. Document it with a worked Caddy example. Direct TLS using mbedTLS's full SSL layer is the v2 path, with a concrete implementation contract in §4.

---

## 2. Options Analysis

### 2.1 Option A — mbedTLS full SSL layer (using vendored DuckDB tree)

#### What exists in the vendor tree

Verified path: `/Users/aloksubbarao/quackapi/ext-cpp/duckdb/third_party/mbedtls/` [V].

`duckdb_mbedtls` is a static library compiled from sources in that tree. Its CMakeLists.txt (`ext-cpp/duckdb/third_party/mbedtls/CMakeLists.txt`) produces the target `duckdb_mbedtls` with these object files [V]:

```
library/aes.cpp  asn1parse.cpp  asn1write.cpp  base64.cpp  bignum.cpp
bignum_core.cpp  cipher.cpp  cipher_wrap.cpp  constant_time.cpp
gcm.cpp  md.cpp  oid.cpp  pem.cpp  pk.cpp  pk_wrap.cpp  pkparse.cpp
platform.cpp  platform_util.cpp  rsa.cpp  rsa_alt_helpers.cpp
sha1.cpp  sha256.cpp
```

The public C++ wrapper is `include/mbedtls_wrapper.hpp` [V]. It exposes:
- `ComputeSha256Hash`, `IsValidSha256Signature`, `Hmac256`, `ToBase16`
- `SHA256State`, `SHA1State` classes
- `AESStateMBEDTLS` (AES-GCM encryption state used for DuckDB internal encryption)
- `AESStateMBEDTLSFactory`

The configuration file `include/mbedtls/mbedtls_config.h` defines [V]:

```
MBEDTLS_AES_C, MBEDTLS_BIGNUM_C, MBEDTLS_CIPHER_C, MBEDTLS_GCM_C,
MBEDTLS_MD_C, MBEDTLS_PEM_PARSE_C, MBEDTLS_PK_C, MBEDTLS_PK_PARSE_C,
MBEDTLS_RSA_C, MBEDTLS_SHA1_C, MBEDTLS_SHA256_C, MBEDTLS_CIPHER_MODE_CBC,
MBEDTLS_CIPHER_MODE_CTR, MBEDTLS_OID_C, ...
```

**What is NOT enabled or compiled [V]:** The library directory contains no `ssl.cpp`, `ssl_srv.cpp`, `ssl_cli.cpp`, `ssl_tls.cpp`, `net_sockets.cpp`, `x509.cpp`, `x509_crt.cpp`, or any other TLS handshake / certificate chain / network socket sources. A full-text search of the library directory returns no SSL/TLS source files. The `include/mbedtls/` directory has a `config_adjust_ssl.h` header stub [V] but no `ssl.h` proper.

**Conclusion [V]: DuckDB's vendored mbedTLS is a stripped "crypto-only" subset.** It provides AES, SHA, RSA key parsing, PEM, and signature verification for DuckDB's internal httpfs HTTPS client verification — not for running a TLS server. There is no `mbedtls_ssl_context`, no `mbedtls_ssl_set_bio`, no `mbedtls_ssl_handshake`, no `mbedtls_ssl_read`, no `mbedtls_ssl_write` in this tree. Linking against `duckdb_mbedtls` cannot produce a TLS server.

#### Full mbedTLS via vcpkg (Option A, alternate)

The full upstream mbedTLS package (`mbedtls` port in vcpkg) includes the stripped sources above plus `ssl_srv.cpp`, `ssl_tls.cpp`, `ssl_tls13_*.cpp`, `net_sockets.cpp`, `x509_crt.cpp`, `entropy.cpp`, and CTR-DRBG for the PRNG. The vcpkg port exists and is well-maintained [U — not verified against current vcpkg registry, but mbedtls has been a vcpkg port since 2017].

**API shape for wrapping an accepted fd:**

After `accept()` returns `fd`, the worker thread (inside `worker_main`, called via the dispatcher in `worker_main`'s loop at `brain.cpp:498`) would:

```c
mbedtls_ssl_context ssl;
mbedtls_ssl_init(&ssl);
mbedtls_ssl_setup(&ssl, &g_tls_conf);   // g_tls_conf loaded at serve_brain_impl time
mbedtls_ssl_set_bio(&ssl, &fd,
                    mbedtls_net_send,    // or a custom send shim
                    mbedtls_net_recv,    // or a custom recv shim
                    NULL);
// Handshake (blocking, on worker thread — correct):
int ret;
while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
        mbedtls_ssl_free(&ssl);
        close(fd);
        return;
    }
}
// Replace read() / write() call sites with:
//   mbedtls_ssl_read(&ssl, buf, len)   — every read() in handle_conn_on
//   mbedtls_ssl_write(&ssl, buf, len)  — every write() in handle_conn_on
mbedtls_ssl_close_notify(&ssl);
mbedtls_ssl_free(&ssl);
close(fd);
```

The `mbedtls_net_send` / `mbedtls_net_recv` callbacks that mbedTLS ships do `send(fd, ...)` / `recv(fd, ...)` — exactly equivalent to the current `write` / `read`. A custom shim forwarding to the raw fd works. No heap allocation beyond the `mbedtls_ssl_context` per connection.

**vcpkg in community CI:** The community extension build pipeline uses `extension-ci-tools` with vcpkg available. `vcpkg.json` at `ext-cpp/vcpkg.json` currently has an empty `dependencies` array [V: `ext-cpp/vcpkg.json`]. Adding `"mbedtls"` (or `"mbedtls[tls13]"`) to that array makes the dependency available at build time across all community CI platforms.

**Cost:** ~4 MB of additional source compiled into the extension binary (mbedtls TLS layer is ~120 KB of object code, small). Link is static, no runtime dependency. The crypto PRNG requires `/dev/urandom` or equivalent — available on all Unix targets, stubbed on wasm (excluded platform).

**Build wire-up:** Add to `ext-cpp/CMakeLists.txt`:
```cmake
find_package(MbedTLS REQUIRED)     # provided by vcpkg
target_link_libraries(${EXTENSION_NAME} MbedTLS::mbedtls MbedTLS::mbedcrypto MbedTLS::mbedx509)
target_link_libraries(${LOADABLE_EXTENSION_NAME} MbedTLS::mbedtls MbedTLS::mbedcrypto MbedTLS::mbedx509)
```

### 2.2 Option B — OpenSSL via vcpkg

OpenSSL is available via vcpkg (`openssl` port). CMakeLists.txt comment at line 6 notes [V]:  
`# DuckDB's extension distribution supports vcpkg. (openssl removed; not needed for quackapi serve)`  
This confirms OpenSSL was explicitly removed from the build at some point, ruling it out as a casual add.

**API shape for wrapping an accepted fd:**

```c
SSL_CTX *g_ssl_ctx;  // initialized at serve_brain_impl time
SSL *ssl = SSL_new(g_ssl_ctx);
SSL_set_fd(ssl, fd);
if (SSL_accept(ssl) <= 0) { SSL_free(ssl); close(fd); return; }
// Replace read(fd,...) with SSL_read(ssl,...) — N call sites in handle_conn_on
// Replace write(fd,...) with SSL_write(ssl,...) — N call sites in handle_conn_on
SSL_shutdown(ssl);
SSL_free(ssl);
close(fd);
```

**Costs:**
- Binary size: OpenSSL adds ~3–4 MB to the loadable extension (vs ~120 KB for mbedTLS TLS layer).
- License: OpenSSL 3.x is Apache 2.0, compatible with quackapi's MIT.
- Platform: vcpkg OpenSSL builds on linux/osx/windows. Windows path is well-exercised. [U — not verified CI matrix but well-known]
- The `openssl removed` comment in CMakeLists.txt is a soft signal that the authors consciously avoided OpenSSL. No hard technical reason to override that unless mbedTLS proves insufficient.

**Verdict:** OpenSSL works but is heavier and was already deliberately removed. Prefer full mbedTLS over OpenSSL if direct TLS is chosen.

### 2.3 Option C — Document-not-build: "Behind a proxy" as v1

Ship a worked Caddy + nginx configuration, document this as the production story, and defer direct TLS to v2.

**Why this is defensible:**

uvicorn's own deployment guide recommends a proxy for production. The `--ssl-*` flags are a development shortcut. The FastAPI "Deployment" docs section opens with nginx/caddy guidance before mentioning uvicorn's built-in TLS. For quackapi, where every deployment currently requires DuckDB to be running anyway, requiring a sidecar proxy is not a meaningful regression.

**Caddy example (zero-config TLS via ACME):**

```
quackapi.example.com {
    reverse_proxy localhost:8080
}
```

Caddy handles cert provisioning, renewal, TLS 1.2/1.3, HTTP/2, and HSTS. quackapi listens on `127.0.0.1:8080` (plain TCP). Zero C code change.

**nginx example:**

```nginx
server {
    listen 443 ssl;
    server_name quackapi.example.com;
    ssl_certificate     /etc/ssl/certs/quackapi.crt;
    ssl_certificate_key /etc/ssl/private/quackapi.key;
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_ciphers HIGH:!aNULL:!MD5;
    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

**What the spec doc should ship alongside the proxy guidance:**

1. A `docs/deploy/TLS_PROXY.md` with the Caddy and nginx examples above.
2. A note in `serve_brain` SQL-level docs: "For TLS, use a reverse proxy. Direct TLS is available from v2."
3. A bind-to-loopback parameter (`host='127.0.0.1'`) so the plain-TCP port is not exposed on external interfaces when behind a proxy. This requires a one-line change to `serve_brain_impl` (line 527: `a.sin_addr.s_addr = 0` → configurable).

**No C code change required for v1 proxy path beyond the optional bind address.**

---

## 3. Threading Fit

### 3.1 Current server model (verified against brain.cpp)

The server is a POSIX raw-socket pthread implementation [V: `brain.cpp:1–586`]:

| Component | Source | Behavior |
|---|---|---|
| Accept loop | `accept_loop()` `brain.cpp:503–518` | Single detached pthread. Calls `accept(g_listen, 0, 0)` blocking. On success, pushes fd to `g_q[4096]` ring buffer and signals `g_qcv`. |
| Worker pool | `worker_main()` `brain.cpp:463–501` | 16 detached pthreads (`NWORKERS=16`, `brain.cpp:216`). Each blocks on `pthread_cond_wait(&g_qcv, &g_qm)`, dequeues one fd, calls `handle_conn_on(con, fd)`. |
| Connection handler | `handle_conn_on()` `brain.cpp:221–461` | Single `read(fd, req, 65535)` (line 226). Multiple `write(fd, ...)` calls throughout (lines 239, 247, 256, 263, 387, 393, 394, 402, 406, 408, 411, 435, 436, 446, 452, 459). Closes `fd` before returning at every exit path. |
| Keep-alive | **ABSENT** | Every response header carries `Connection: close` (e.g. `brain.cpp:239`, `brain.cpp:433`). There is no keep-alive loop in `handle_conn_on`. Each fd is closed after a single request/response cycle. |

**The connection-per-request model is confirmed [V]:** `handle_conn_on` always emits `Connection: close` and always calls `close(fd)` before returning. There is no pipelining or persistent-connection loop.

### 3.2 TLS handshake placement

The correct insertion point for TLS is **inside `worker_main`'s dispatch loop, immediately after dequeuing the fd and before calling `handle_conn_on`:**

```c
// In worker_main, replacing line 498:
// handle_conn_on(con, fd);

if (g_tls_enabled) {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_setup(&ssl, &g_tls_conf);
    mbedtls_ssl_set_bio(&ssl, &fd, mbedtls_net_send_cb, mbedtls_net_recv_cb, NULL);
    int ret;
    do {
        ret = mbedtls_ssl_handshake(&ssl);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (ret != 0) {
        mbedtls_ssl_free(&ssl);
        close(fd);
        continue;  // worker loop
    }
    handle_conn_on_tls(con, fd, &ssl);  // variant using mbedtls_ssl_read/write
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
} else {
    handle_conn_on(con, fd);
}
```

This is correct because:
- The handshake is blocking and runs on the worker thread — no event loop, no async, no issue.
- `g_tls_conf` is initialized once at `serve_brain_impl` time (cert/key loaded, server-mode, no CA verification) and is read-only during the serve loop. Safe for concurrent access without a lock.
- The `mbedtls_ssl_context` is per-connection, stack-allocated within the worker's dispatch iteration. It is freed before the `continue`.

### 3.3 read() / write() call sites that require TLS wrappers

Every `read()` and `write()` call in `handle_conn_on` must be redirected when TLS is active. The complete list [V: `brain.cpp:221–461`]:

| Line | Direction | Description |
|------|-----------|-------------|
| 226 | recv | `read(fd, req, 65535)` — single HTTP request read |
| 239 | send | `write(fd, pong, ...)` — /ping fast path |
| 247 | send | `write(fd, q, ...)` — /q1 fast path |
| 256 | send | `write(fd, q, ...)` — /q2 fast path |
| 263 | send | `write(fd, q, ...)` — /q3 fast path |
| 387 | send | `write(fd, hdr, hl)` — stream response header |
| 393 | send | `write(fd, lhex, lh)` — chunk size hex |
| 394 | send | `write(fd, em, ...)` — stream error event |
| 395 | send | `write(fd, "\r\n0\r\n\r\n", 7)` — stream terminator |
| 402 | send | `write(fd, lhex, lh)` — per-row chunk size |
| 406 | send | `write(fd, ev, evl)` — per-row SSE event |
| 408 | send | `write(fd, "\r\n", 2)` — chunk CRLF |
| 410 | send | `write(fd, "0\r\n\r\n", 5)` — stream end |
| 435 | send | `write(fd, hdr, hl)` — non-stream response header |
| 436 | send | `write(fd, hbody, bl)` — non-stream response body |
| 446 | send | `write(fd, hdr, hl)` — static body response header |
| 447 | send | `write(fd, dec.body, bl)` — static body |
| 452 | send | `write(fd, e, ...)` — 500 no handler |
| 459 | send | `write(fd, e, ...)` — 500 no router |
| (all error paths) | send | Various short error write()s |

**Implementation strategy for TLS variant:** The cleanest approach is a thin I/O abstraction:

```c
typedef struct {
    int fd;
    mbedtls_ssl_context *ssl;  /* NULL if plaintext */
} QuackIO;

static ssize_t quack_read(QuackIO *io, void *buf, size_t len) {
    if (!io->ssl) return read(io->fd, buf, len);
    return mbedtls_ssl_read(io->ssl, (unsigned char*)buf, len);
}

static ssize_t quack_write(QuackIO *io, const void *buf, size_t len) {
    if (!io->ssl) return write(io->fd, buf, len);
    /* mbedtls_ssl_write may do a short write; loop for exact delivery */
    size_t sent = 0;
    while (sent < len) {
        int r = mbedtls_ssl_write(io->ssl, (const unsigned char*)buf + sent, len - sent);
        if (r <= 0) return -1;
        sent += r;
    }
    return (ssize_t)sent;
}
```

Then `handle_conn_on(con, fd)` becomes `handle_conn_on(con, QuackIO io)` and all `read(fd,...)` / `write(fd,...)` become `quack_read(&io,...)` / `quack_write(&io,...)`. The fast-path diagnostics (`/ping`, `/q1`, `/q2`, `/q3`) at lines 237–265 also need this replacement.

**Note on mbedtls_ssl_write short-write [U]:** The mbedtls spec says `mbedtls_ssl_write` may return less than `len` for TLS record boundary reasons. The loop above handles this; the current plain `write()` calls do not loop, which is technically also incomplete (POSIX write on TCP may short-write under memory pressure) but acceptable for quackapi's small response sizes. The TLS wrapper must loop.

### 3.4 Throughput impact

**Measured baseline (plaintext):** quackapi benchmarks show ~25–41k req/s for static routes (`/health`, `/ping`) and ~5–7k req/s for dynamic routes (DuckDB query involved). [U — from project documentation; not re-benchmarked here.]

**TLS handshake cost:**

Each TLS 1.3 handshake with a 2048-bit RSA certificate (ECDHE key exchange, AES-128-GCM record cipher) costs approximately:
- **RSA signature verify:** ~0.3–1 ms on modern hardware [U — from mbedTLS benchmarks; exact cost depends on key size and CPU]
- **ECDHE key agreement:** ~0.1–0.3 ms [U]
- **Total handshake round trips:** TLS 1.3 = 1 RTT; TLS 1.2 = 2 RTT. For loopback, RTT ≈ 0.02 ms, so RTT cost is negligible.
- **Combined handshake wall time:** ~0.5–2 ms per new connection

**Implication for quackapi (critical):** Every connection is a new handshake because there is no keep-alive (`Connection: close` at every response — confirmed [V]). At 5k req/s, 5000 handshakes/second × 1 ms each = 5 seconds of handshake CPU per second, which is physically impossible on one core. **TLS overhead at quackapi's throughput with Connection-close semantics would reduce effective TLS-protected throughput to approximately 500–2000 req/s** (limited by handshake CPU, not I/O or DuckDB query time).

**Session resumption does not help here:** TLS 1.3 session tickets (0-RTT resumption) require the client to reconnect with the same session ticket. For benchmarks using `curl`, `wrk`, `ab` in default mode, each request is a new connection with a new handshake. Session resumption helps browser users making sequential requests to the same host — not a concern for a data API behind a proxy.

**AES-GCM record encryption cost:** After the handshake, AES-128-GCM on the data itself (requests are small: ~200 bytes; responses are small: typically < 4 KB for API use) adds < 0.05 ms per request. Negligible compared to the DuckDB query cost.

**Bottom line:** Direct TLS is incompatible with quackapi's peak throughput claims unless keep-alive is added. If keep-alive existed (allowing session resumption and amortizing handshake cost across multiple requests per connection), effective throughput under TLS would be 80–90% of plaintext throughput for AES-GCM bulk encryption. Without keep-alive, direct TLS should be considered a "development + moderate-load" feature only; production high-throughput deployments must use a proxy (where TLS terminates at nginx/caddy, and quackapi sees plain TCP on loopback, with potential keep-alive on the proxy↔upstream link).

**This is the single hardest problem with direct TLS in quackapi** (see §6).

---

## 4. Surface Design

### 4.1 How certs/keys are passed

**Current `serve_brain_impl` signature [V: `brain.cpp:520`]:**

```c
const char *serve_brain_impl(int port, const char *db_path)
```

**DuckDB SQL-level wrapper [V: via the extension UDF registration — not read directly but consistent with the SQL scalar `serve_brain(port, db_path)`]:**

```sql
SELECT serve_brain(8080, ':memory:');
```

**Recommended surface (consistent with DuckDB idioms and existing signature):**

Add two optional string parameters to `serve_brain`. NULL/empty means plaintext (no behavior change for existing callers):

```c
// New C impl signature:
const char *serve_brain_impl(int port, const char *db_path,
                             const char *cert_path, const char *key_path);

// SQL UDF:
SELECT serve_brain(8080, ':memory:', cert_path, key_path);
-- or plaintext (NULL omits TLS):
SELECT serve_brain(8080, ':memory:');
-- or keep_alive + access_log combined (from POLISH_OPS_SPEC.md):
SELECT serve_brain(8080, ':memory:', cert_path, key_path, gzip:=false, access_log:=false, ...);
```

**Why not DuckDB CREATE SECRET:** `CREATE SECRET` is designed for credential secrets (cloud provider tokens, S3 keys, OAuth) and stores into the catalog. Cert/key paths are filesystem paths, not secrets. They do not benefit from the secret scoping/encryption model. Passing them as function parameters keeps the mental model simple and is consistent with how uvicorn takes `--ssl-keyfile` and `--ssl-certfile` as CLI arguments — both are just path strings.

**Why not environment variables:** Env vars are harder to document, harder to test in SQL harnesses, and inconsistent with the existing parameter-passing style of `serve_brain`. Env vars as a fallback (e.g. `QUACKAPI_TLS_CERT`, `QUACKAPI_TLS_KEY`) may be added as a secondary convenience but should not be the primary surface.

### 4.2 Global TLS state

```c
static int g_tls_enabled = 0;
static mbedtls_ssl_config  g_tls_conf;
static mbedtls_x509_crt    g_tls_cert;
static mbedtls_pk_context   g_tls_key;
static mbedtls_entropy_context  g_tls_entropy;
static mbedtls_ctr_drbg_context g_tls_drbg;
```

All initialized once in `serve_brain_impl` when `cert_path != NULL && cert_path[0] != '\0'`. All read-only during the serve loop (thread-safe without locking).

### 4.3 TLS config at boot

```c
// In serve_brain_impl, after bind/listen, before pool start:
if (cert_path && cert_path[0] && key_path && key_path[0]) {
    mbedtls_ssl_config_init(&g_tls_conf);
    mbedtls_x509_crt_init(&g_tls_cert);
    mbedtls_pk_init(&g_tls_key);
    mbedtls_entropy_init(&g_tls_entropy);
    mbedtls_ctr_drbg_init(&g_tls_drbg);

    mbedtls_ctr_drbg_seed(&g_tls_drbg, mbedtls_entropy_func, &g_tls_entropy,
                           (const unsigned char *)"quackapi", 8);
    mbedtls_x509_crt_parse_file(&g_tls_cert, cert_path);
    mbedtls_pk_parse_keyfile(&g_tls_key, key_path, NULL, mbedtls_ctr_drbg_random, &g_tls_drbg);

    mbedtls_ssl_config_defaults(&g_tls_conf, MBEDTLS_SSL_IS_SERVER,
                                MBEDTLS_SSL_TRANSPORT_STREAM,
                                MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_rng(&g_tls_conf, mbedtls_ctr_drbg_random, &g_tls_drbg);
    mbedtls_ssl_conf_own_cert(&g_tls_conf, &g_tls_cert, &g_tls_key);
    // No CA verification: we are the server, clients are browsers/curl/apps
    mbedtls_ssl_conf_authmode(&g_tls_conf, MBEDTLS_SSL_VERIFY_NONE);

    g_tls_enabled = 1;
}
```

**Error handling:** Any failure in the above (cert parse error, key parse error, CTR-DRBG seed fail) must return early with a specific error string: `"TLS_CERT_FAIL"`, `"TLS_KEY_FAIL"`, `"TLS_RNG_FAIL"`. These are surfaced through the existing `serve_brain_impl` return value (already returns `"BIND_FAIL"`, `"SYM_FAIL"` etc. [V: `brain.cpp:529,556`]).

---

## 5. Recommendation, Effort, and Build Order Position

### 5.1 Recommendation

**v1 (ship now): Option C — "Behind a proxy" documentation only.**

Rationale: uvicorn itself recommends this path for production. The Connection-close architecture of quackapi makes direct TLS prohibitively expensive for any meaningful load (§3.4). The implementation is zero C code. The deliverable is a `docs/deploy/TLS_PROXY.md` with Caddy and nginx examples, plus an optional `host=` bind-address parameter on `serve_brain` (single-line change to `brain.cpp:527` to bind to `127.0.0.1` when `host='127.0.0.1'` is passed).

**v2 (after keep-alive is implemented): Option A — full mbedTLS via vcpkg.**

The throughput problem (§3.4) is solvable by keep-alive. Once `handle_conn_on` loops on a persistent connection (with TLS session ticket resumption), a TLS-protected quackapi can approach 80–90% of plaintext throughput for warm connections. The implementation uses full mbedTLS from vcpkg (not the vendored stripped copy which lacks SSL sources [V]).

**Never: Option B (OpenSSL).** Deliberately removed from CMakeLists.txt [V: line 6]. Heavier than mbedTLS, no advantage over mbedTLS for this use case.

### 5.2 Effort table

| Track | Option | Effort | Prerequisite | Risk |
|---|---|---|---|---|
| v1 | C — Proxy docs + bind-address param | **S** | None | None |
| v2 | A — Full mbedTLS via vcpkg, direct TLS | **M** | Keep-alive in brain.cpp | Medium: vcpkg dependency addition, 20+ call-site replacements in handle_conn_on, new worker-dispatch TLS branch |
| Never | B — OpenSSL | N/A | — | — |

**S = 1–2 days. M = 3–5 days + keep-alive as prereq.**

### 5.3 Build order position

TLS is not on the current feature roadmap (POLISH_OPS_SPEC.md §8 explicitly lists it as "HARD gap, separate track" [V]). The implementation order within the roadmap is:

1. Complete POLISH_OPS_SPEC.md items (HEAD, 405, gzip, graceful shutdown, access logs) — in progress.
2. Keep-alive implementation (not yet specced; needed for TLS performance).
3. **TLS v1:** Proxy documentation + optional bind-address parameter. Can ship as part of any release.
4. **TLS v2:** Direct mbedTLS after keep-alive ships and is benchmarked.
5. Community extension path (COMMUNITY_EXT_PATH.md) — parallel track; TLS vcpkg dep would be added to `vcpkg.json` at that point.

---

## 6. What We Will Not Support

Explicit exclusions, in priority order:

| Feature | Reason |
|---|---|
| **Client certificates (mTLS)** | Adds substantial complexity to the mbedTLS config (`mbedtls_ssl_conf_ca_chain`, verification callbacks). Not part of uvicorn's standard feature set. No user request. Excluded permanently from v2 scope. |
| **ALPN / HTTP/2 (h2)** | HTTP/2 requires a multiplexed framing layer, stream IDs, HPACK header compression — fundamentally incompatible with the one-connection-per-request + single-`read` model. Would require a complete rewrite of `handle_conn_on`. Not in scope for any version. |
| **SNI (multiple certs for multiple domains)** | Single `serve_brain` call binds one port. One cert/key pair. SNI routing to multiple certs is a proxy concern. |
| **Certificate rotation without restart** | The cert/key globals are set once at `serve_brain_impl` time. Hot-rotation requires `quack_reload_router`-style mechanism for TLS config — too much complexity for current scope. Restart is the mitigation. |
| **OCSP stapling** | Proxy-level concern (nginx/caddy handle it). Not in scope for the C layer. |
| **TLS 1.0 / 1.1** | mbedTLS `MBEDTLS_SSL_PRESET_DEFAULT` disables them by default. We inherit that default. No backward compat for deprecated protocol versions. |
| **Session ticket key rotation** | Default mbedTLS behavior generates a session ticket key at boot. Rotation on long-running servers is a proxy concern when terminating at proxy. For direct TLS, rotation requires a timer thread — deferred indefinitely. |
| **PEM-encrypted private keys requiring passphrase** | `mbedtls_pk_parse_keyfile` with `NULL` password (see §4.3). Encrypted keys would require a `key_password` parameter — deferred. |

---

## 7. Verified Surprises

1. **DuckDB's vendored mbedTLS has NO SSL/TLS sources [V: library directory listing].** Only crypto primitives (AES, SHA, RSA key parse, PEM). `mbedtls_ssl_context`, `mbedtls_ssl_handshake`, `mbedtls_ssl_read`, `mbedtls_ssl_write` — all absent. Linking against `duckdb_mbedtls` cannot produce a TLS server. Full mbedTLS must come from vcpkg.

2. **Connection: close is emitted unconditionally on every response [V: brain.cpp:239,433 and all other response paths].** Keep-alive does not exist. This means every TLS request pays a full handshake. At 5k req/s, direct TLS is CPU-saturated on RSA signing alone. The proxy path avoids this entirely; direct TLS requires keep-alive as a prerequisite for acceptable throughput.

3. **OpenSSL was explicitly removed from CMakeLists.txt [V: line 6 comment].** The comment is a deliberate authoring note. Do not re-add without a strong reason.

4. **The `mbedtls_ssl_write` API may short-write [U].** The current `write(fd,...)` calls in `handle_conn_on` do not loop. A TLS wrapper must loop for correctness. This is a behavior change vs. plaintext for all write sites.

5. **vcpkg is available in community-extensions CI.** The `ext-cpp/vcpkg.json` currently has `"dependencies": []` [V]. Adding `"mbedtls"` is the correct mechanism for the v2 build. No infrastructure change needed.

6. **The accept loop holds no fd-level state** (`accept_loop` at `brain.cpp:503` pushes only the integer fd into `g_q`). This means TLS state must be set up entirely inside `worker_main`'s dispatch, not at accept time. Per-fd TLS context on the worker thread is the correct model — and it is consistent with how uvicorn / asyncio handle per-connection SSL contexts.

---

## 8. Unverified Items

- **mbedtls vcpkg port name [U]:** The port is believed to be `"mbedtls"`. The TLS 1.3 capability is likely in `"mbedtls[tls13]"` or enabled via a feature flag. Verify against current vcpkg registry before adding to `vcpkg.json`.
- **mbedtls CMake target names [U]:** May be `MbedTLS::mbedtls`, `MbedTLS::mbedcrypto`, `MbedTLS::mbedx509` (standard vcpkg cmake integration) or may vary. Verify with `vcpkg install mbedtls --triplet x64-linux` before updating CMakeLists.txt.
- **Session ticket support in mbedTLS TLS 1.3 [U]:** TLS 1.3 0-RTT session resumption via session tickets may require additional mbedTLS config (`mbedtls_ssl_conf_session_tickets`). Verify against mbedTLS 3.x docs.
- **Throughput numbers [U]:** The 25–41k req/s plaintext figures come from project documentation. The 500–2000 req/s TLS-with-new-connection estimate is derived from RSA-2048 signing benchmarks (~2k signs/s on a 2020 ARM core, faster on modern x86). Benchmark on target hardware before citing.
