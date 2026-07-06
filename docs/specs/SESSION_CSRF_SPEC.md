---
title: "SESSION_CSRF_SPEC — first-class sessions + CSRF as DDL"
subtitle: "Server-side sessions (a table), signed cookies via crypto ext, synchronizer CSRF (we have state), flash messages, integration with CREATE AUTH so policies stay scheme-agnostic."
author: quackapi
date: 2026-07-05
---

# SESSION_CSRF_SPEC — first-class sessions + CSRF

**Status:** build-ready spec (Wave A; addresses the single highest-upvoted FastAPI feature request in history — #754, 75+ 👍, closed unresolved; also #5796 flash; plus bolt-on ecosystem report §8).

**References (read first):** docs/ROADMAP_10M.md (§5 rank 6), docs/research/fastapi-pain-2026-07-02/07-bolton-ecosystem.md §8 (sessions/CSRF details + defection evidence), docs/specs/CREATE_POLICY_AUTH_SPEC.md (auth integration, claims MAP shape, CREATE SECRET reuse, constant-time compare requirement, house style), framework.sql (cookie extraction from headers._cookies already parsed by C brain.cpp, route_headers for Set-Cookie, handle_request), ext-cpp/src/quackapi_brain.cpp (cookie parsing at ~2949, Set-Cookie via route_headers, static response headers path).

**Thesis**

FastAPI (and Starlette) sessions are stateless signed cookies only (itsdangerous). Logout does not revoke anything; there is no server-side store; revocation, regeneration on privilege change, and true "logged out" semantics are impossible without bolting on starsessions + Redis + yet another abandoned micro-package. CSRF is declined ("use a third-party"). The highest-👍 feature issue ever is the direct result. Users migrate to Django-Ninja or Litestar explicitly citing "storing the session in the database had only half-finished solutions."

quackapi has server state by construction (everything is tables). Therefore:

- Sessions **are** a table (`CREATE SESSION STORE` or equivalent DDL that owns a `quackapi_sessions` backing table).
- Signed cookies use the **same crypto_hmac** primitive already verified in CREATE_POLICY_AUTH_SPEC + compose receipts (reuses CREATE SECRET).
- CSRF can (and should) be the stronger synchronizer-token pattern because we have the session row.
- Flash messages are trivial (one-request values in the session row or a companion flash table) and kill another long-standing FastAPI complaint (#5796).
- `claims` MAP shape is identical to JWT bearer, so `CREATE POLICY` predicates and handlers do not care which scheme authenticated the request.

We add the batteries; we do not invent new crypto.

## 1. `CREATE SESSION STORE` (or equivalent DDL)

**Grammar sketch (style mirrors CREATE AUTH / CREATE POLICY; exact token details refined with the DDL fleet):**

```sql
CREATE SECRET session_signing (TYPE quackapi_key, SECRET '32-bytes-or-more-for-hmac');

CREATE SESSION STORE sessions
  SECRET session_signing
  COOKIE_NAME 'sid'
  COOKIE_PATH '/'
  COOKIE_SAMESITE 'Lax'          -- or Strict
  COOKIE_SECURE false            -- see edges; becomes true under TLS
  EXPIRES 86400                  -- seconds; or '7 days' sugar
  -- optional: TABLE sessions (to name/override the backing table)
;
```

**Backed by an ordinary table (introspectable, mutable by DDL + DML):**

```sql
-- created by the DDL (or user can manage; the store owns the contract)
CREATE TABLE IF NOT EXISTS quackapi_sessions (
  id VARCHAR PRIMARY KEY,           -- opaque session id (uuid or random)
  subject VARCHAR,                  -- user id / principal from claims
  claims JSON,                      -- the verified claims MAP serialized; on read -> MAP
  created_at TIMESTAMP,
  expires_at TIMESTAMP,
  revoked_at TIMESTAMP,             -- NULL = live
  csrf_token VARCHAR,               -- for synchronizer pattern
  flash JSON                        -- one-shot flash values { "key": "msg" }
);
```

**Cookie value format (signed):**

`sid|exp|sig`

Where:
- `sid` = the session id (url-safe, no |)
- `exp` = unix seconds
- `sig` = hex( crypto_hmac('sha2-256', secret, sid + '|' + exp ) )

On Set-Cookie:
```
Set-Cookie: sid=sess_abc123|1750000000|69db38...40c; HttpOnly; SameSite=Lax; Path=/; Max-Age=86400
```

(Additional attrs from the STORE clause.)

**Lifecycle (all expressible as SQL + the C cookie header path):**

1. **Login / session creation (after successful password or other primary auth):** INSERT a row with fresh id, claims, expires, a fresh csrf_token, flash=NULL. Then emit the signed cookie via route_headers (or a helper that the auth flow uses). The response can also carry the csrf_token in a parallel non-HttpOnly cookie or in the JSON body for the SPA/JS client to read once.

2. **Request with cookie:** C layer already parses cookies into `headers._cookies`. The session auth scheme (see §2) extracts `sid`, verifies the sig with constant-time compare (see verified mechanism), loads the row, rejects if revoked or expired, materializes `claims` from the row (identical shape to JWT), and proceeds. On successful read, optionally do a lazy expiry sweep (DELETE or mark where expires_at < now()).

3. **Session regeneration on privilege change:** On role elevation, password change, etc., the handler (or a helper macro) does:
   - INSERT new row copying claims + new id + new csrf_token + fresh expires.
   - UPDATE old row SET revoked_at = now().
   - Emit new Set-Cookie (overwriting the old sid).
   This is exactly what stateless signed-cookie schemes cannot do.

4. **Logout = DELETE (revocation actually works):** UPDATE ... SET revoked_at=now() (or hard DELETE). Optionally emit a Set-Cookie with Max-Age=0 or a clearing value. Contrast with FastAPI: the old signed cookie remains valid forever because there is no server record to revoke.

5. **Expiry sweep:**
   - Primary: lazy on read (in the verify path: if now() > expires_at, treat as 401 and optionally prune the row).
   - Secondary (Wave C): ride `CREATE CRON` (once it exists) for a background `DELETE FROM quackapi_sessions WHERE expires_at < now() AND revoked_at IS NULL;`.
   Decision: lazy-first is sufficient and cheap for v1; document the cron as the production sweeper.

**Verified mechanism — crypto / cookie (real duckdb -unsigned probes)**

Crypto ext + hmac already works for signing (compose.sql receipt + AUTH_SPEC).

Probe 1 — JWT verify (reproduced exactly from CREATE_POLICY_AUTH_SPEC):

```sql
-- transcript from 2026-07-05 run in /tmp
WITH t AS (
  SELECT 'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2MjM5MDIyfQ' AS signing_input,
         'SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c' AS sig_b64url,
         'your-256-bit-secret' AS secret
)
SELECT crypto_hmac('sha2-256', secret, signing_input) =
       from_base64(replace(replace(sig_b64url,'-','+'),'_','/') || repeat('=', (4 - (length(sig_b64url) % 4)) % 4))
       AS signature_valid
FROM t;
-- result: signature_valid = true
```

Probe 2 — signed cookie value construction (sid|exp|sig):

```sql
-- transcript
WITH params AS (
  SELECT 'sess_abc123' AS sid, 1750000000 AS exp, 'server-secret-32byteslongatleast' AS secret
),
cookie_val AS ( SELECT sid || '|' || exp AS to_sign, * FROM params ),
sig AS ( SELECT lower(hex( crypto_hmac('sha2-256', secret, to_sign) )) AS hmac_sig, * FROM cookie_val )
SELECT to_sign, hmac_sig, sid || '|' || exp || '.' || hmac_sig AS cookie_value FROM sig;
-- result:
-- to_sign = sess_abc123|1750000000
-- hmac_sig = 69db385f91aa50a8404c0784415437c83817160ef92a2bfed380efc8ab30640c
-- cookie_value = sess_abc123|1750000000.69db385f91aa50a8404c0784415437c83817160ef92a2bfed380efc8ab30640c
```

Probe 3 — DuckDB equality compare semantics (for constant-time note):

```sql
-- transcript
WITH t AS (
  SELECT crypto_hmac('sha2-256', 'k', 'p1') AS a,
         crypto_hmac('sha2-256', 'k', 'p1') AS b_eq,
         crypto_hmac('sha2-256', 'k', 'p2') AS b_neq
)
SELECT a = b_eq AS duckdb_eq_match, a = b_neq AS duckdb_eq_nomatch FROM t;
-- duckdb_eq_match = true, duckdb_eq_nomatch = false
-- note emitted: DuckDB blob/string = is length+memcmp, not guaranteed constant-time across all builds
```

**Constant-time compare requirement (same as AUTH_SPEC):** After decoding the sig portion, the comparison `expected == provided` must be constant-time. DuckDB's `=` on BLOB/VARCHAR is length-then-bytes (memcmp after length check). v1 ships a SQL helper `crypto_constant_time_eq(a, b)` (XOR-fold over bytes) or relies on the crypto ext exposing one. If neither, the compare path in the session verifier is a TODO marked "must before public-internet claims." Flag identical to the one in CREATE_POLICY_AUTH_SPEC.

**Set-Cookie attributes:** driven by the STORE clause, emitted via the existing `route_headers` + C write path (framework.sql already documents this for redirects and cookies). HttpOnly and SameSite are non-negotiable for session cookies.

## 2. Integration with CREATE AUTH

Session auth is just another scheme. It produces the **identical `claims` MAP** shape that JWT schemes produce.

```sql
CREATE AUTH cookie AS SESSION (
  STORE sessions,
  SECRET session_signing,
  COOKIE 'sid',           -- name
  HEADER 'Cookie'         -- or implicit
);

-- now a route can declare:
-- (the route registration or a policy layer wires the scheme)
```

On successful verification the auth phase (between route match and policy/handler, per AUTH_SPEC) populates the same `claims` binding used by policies and handlers. A policy written against `claims['sub']` or `claims['role']` works for both Bearer JWT and Cookie Session users. No branching in user code.

The scheme can be mixed: some routes Bearer-only, some cookie (the admin UI), some either. Policies remain oblivious.

Logout/revocation works because the store is the source of truth; stateless JWT revocation still needs the denylist join.

## 3. CSRF

**Choice justified:** Double-submit cookie (stateless HMAC cookie + header must match the value) is the usual answer when you have no server state. We **do** have server state (the session row). Therefore we use the **synchronizer token pattern**:

- On session creation/regen, generate a `csrf_token` (crypto-quality random, stored in the session row).
- The token is sent to the client (readable cookie `csrf_token=...` or in the login response JSON; the admin HTML reads it once).
- For unsafe methods (POST, PUT, PATCH, DELETE, and any route marked mutating), the request **must** include the token in:
  - Header: `X-CSRF-Token: <value>`
  - or form field: `_csrf` / `csrf_token`
- Server: look up the session (or the claims if a session id is present), retrieve the stored token, constant-time compare. Mismatch or missing → 403.
- Double-submit variant is also possible as a fallback (no row lookup for the token), but synchronizer is the primary because we can.

**Exemptions:** Routes authenticated purely via Bearer / API key (no session cookie present) are exempt from CSRF checks. CSRF is a cookie-oriented browser attack.

**Header/form field names:** `X-CSRF-Token` (header) and `csrf_token` (form). Accept either; document the precedence.

**403 shape:** consistent with policy failures — `{"detail": "CSRF token missing or invalid"}` or the standard 403 from the auth layer.

**Methods protected:** all mutating per RFC (POST, PUT, PATCH, DELETE). Safe methods (GET, HEAD, OPTIONS) never require the token (but the token may still be rotated or emitted).

**Implementation notes:** the check lives in the middleware/auth phase (after session cookie verification, before policy). Pure SQL oracle path mirrors it for Tier-1 parity. C hot path does the header extraction (already has cookie + header paths) + the compare.

## 4. Flash messages (one-request session values)

FastAPI #5796 (no flash) is a direct consequence of stateless sessions. We have a row — trivial.

API (sugar over the session table):

```sql
-- inside a handler after successful action
SELECT set_flash({ 'success': 'User created' });

-- in the next request (HTML render or JSON envelope)
SELECT get_flash() AS flash;  -- returns the map, then clears it
```

Storage: in the `flash` JSON column of the session row (or a tiny companion table `session_flashes(session_id, key, value, consumed)` for multi-value). On read, the values are returned and the row is updated to clear them (or marked consumed). One request lifetime.

For the admin UI (and any HTML route): the shell template or layout can read flash on every render and show toasts/banners, then the values are gone.

Cost: one extra column or small table; zero impact on non-session paths.

This is "cheap here" exactly as the task states.

## 5. Tier-1 test plan + C-mirror notes

**Tier-1 (pure SQL oracle + handle_request) must cover:**
- CREATE SESSION STORE emits the expected table + default rows if any.
- Session create → signed cookie value emitted in resp_headers; parse + verify roundtrips with correct claims.
- Tampered sig → 401 (sig mismatch).
- Expired row → 401 (and optional prune).
- Revoked row → 401.
- Logout marks revoked; old cookie now 401.
- Regeneration produces new sid + invalidates old; claims preserved.
- CSRF: mutating request with matching token (header and form) succeeds; mismatch/missing → 403; safe method never triggers.
- Bearer-only route with no cookie never requires CSRF.
- Flash set on request N appears on N+1 exactly once.
- Claims shape identical to a JWT scheme for the same subject (policy predicate passes for both).
- Concurrent sessions for same subject allowed (or configurable later).

**C-mirror notes:** the session verify + CSRF check must be implemented in the C auth phase (parallel to JWT verify) so hot-path cookie sessions do not pay a full SQL dispatch for the lookup + sig check. The pure SQL oracle (Tier-1) and C must produce byte-identical 401/403/claims for the same inputs (existing conformance harness extended with session cases). Cookie parsing already exists; adding the signed-sid logic + table lookup (via the worker con) mirrors the oracle.

**Parity surface:** add cases to the conformance driver for cookie login, protected mutating POST with/without CSRF, flash roundtrip, revocation visibility, regen.

## 6. Honest edges (name them)

- **Cookie auth over non-TLS (until Wave B TLS lands):** HttpOnly + SameSite help, but the cookie (and any CSRF token cookie) travels in cleartext on the network. A MITM can steal the session. Document loudly: "cookie/session auth is localhost-safe or requires TLS (Wave B)". Pure Bearer JWT is the recommended public-internet mechanism until then.
- Revocation and regeneration require a store hit (table read + possible write). This is the price of "logout actually works."
- Session fixation: creation always uses a fresh id; regen on privilege change mitigates.
- Cookie size: claims are JSON in the row, not in the cookie. Cookie only carries the signed id+exp. Good.
- Clock skew: reuse the LEEWAY concept from JWT (AUTH_SPEC) for exp checks.
- Multiple cookies with same name: last-wins or explicit path/domain rules apply; keep simple (one sid per path=/).
- No built-in "remember me" vs session cookie distinction in v1 (all are Max-Age'd; "persistent" is just a longer EXPIRES).
- Concurrent modification of the same session row under high load: ordinary MVCC / last-writer wins; document for advanced users.
- Flash is best-effort for pure-API clients; primarily valuable for HTML/HTMX flows.
- Until a first-class random token function lands, csrf_token generation can use `uuid()` or `hash(random())` + crypto; make it strong.

**Tier-1 + C parity + the existing access-log / graceful shutdown machinery remain the regression net.**

---

**End of SESSION_CSRF_SPEC**
