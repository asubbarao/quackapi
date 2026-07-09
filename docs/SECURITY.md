# quackapi Security Model & Audit Ledger

Audited 2026-07-09 (post sessions/lifecycle/CORS merge). This is the honest ledger:
what the design guarantees, what was found and fixed, and what remains open.
Nothing here is aspirational — every "fixed" entry names the commit and how it was
proven.

## Trust model

- **The app developer is trusted.** Route handlers, policy predicates, lifecycle
  hook SQL, and dependency setup/teardown are DDL-time inputs written by the
  developer. They execute with full database authority by design — the same trust
  FastAPI places in your Python route functions.
- **The HTTP client is untrusted.** Everything arriving over the socket — method,
  path, headers, cookies, body, tokens — is hostile input. The invariants below
  are about that boundary.
- **Crypto is delegated to DuckDB's `crypto` extension** (HMAC-SHA256). quackapi
  deliberately implements no cipher, hash, or comparison primitive of its own.

## Invariants (enforced, with mechanism)

1. **One verification implementation per credential type.** The C worker delegates
   session-cookie verification to the oracle's `_verify_session_cookie` SQL macro
   via a prepared statement, and JWT verification to `_verify_jwt_hs256`. There is
   no second HMAC/parse implementation to drift. (The registry-loading analogue of
   this rule is `quack_load_all_registries` — the CORS-dead-at-boot and DI-drop
   bugs both came from duplicated paths, ext-cpp `dc10558`.)
2. **Constant-time credential comparison at one choke point.**
   `_constant_time_str_equals` compares keyed hashes
   (`hmac(salt,x) = hmac(salt,y)`, public domain-separation salt) so byte-prefix
   timing cannot walk a signature or API key. Used by: JWT signature check,
   API-key lookup, session-cookie signature check, CSRF token compare — oracle and
   C tracks both.
3. **Untrusted values are bound, never spliced.** The session verify and CSRF
   queries in the C worker use prepared-statement binds for cookie value, secret,
   CSRF token, and sid. (Residual: the older JWT path splices with `''`-escaping —
   see Open items.)
4. **Session cookies**: `sid|exp|hmac-sha256` signed value; sid is a server-minted
   random UUID (128-bit) — the client can never choose it, so session fixation has
   no vector. Verification checks signature (constant-time), signed exp, row
   existence, `revoked_at IS NULL`, and server-side `expires_at >= now()` — the
   last enforced on BOTH the standalone macro and the inlined `handle_request`
   copy (`2e4d275` fixed the inline gap: a soft-revoked session could previously
   outlive its shortened row expiry until cookie exp).
5. **Session verification always reads current data.** The worker's auth stage
   runs on its persistent writer connection, not the read replica — revocation is
   effective on the next request, with no replica-staleness window. (Replicas only
   execute the already-authorized handler.)
6. **CSRF (synchronizer token)**: per-session random token; unsafe methods
   (non-GET/HEAD/OPTIONS) on session-authenticated schemes require `X-CSRF-Token`
   matching the session row via constant-time compare; failure forces 403. Bearer
   and API-key schemes are exempt (no ambient credential). Oracle and C parity.
7. **Cookie attributes**: `HttpOnly` always; `Path`, `SameSite` (default `Lax`),
   `Secure`, `Max-Age` from the session store config (`CREATE SESSION STORE`).
8. **401 vs 403 discipline**: missing/invalid credential → 401; valid credential
   failing policy → 403 (the FastAPI #10177 complaint, done right). Verified in
   the 11-case A1 matrix and the 7-case session matrix.
9. **Secrets are declared once.** `CREATE AUTH x AS SESSION (STORE 's')` resolves
   the signing secret from the named store at DDL time instead of asking the
   developer to repeat it.
10. **Loopback by default.** `serve_brain(port, path)` binds 127.0.0.1; exposing
    on all interfaces requires the explicit 3-arg `'0.0.0.0'` form.
11. **No dual DatabaseInstance on one file.** `serve_brain` called from the
    session that owns the served file shares that instance via a connection
    factory instead of a second `duckdb_open`. Two same-process instances on one
    file bypass fcntl locks and the last checkpoint silently destroys the other's
    writes — proven live (HTTP-created rows vanished while shell DDL survived),
    fixed and re-proven in `dc10558`.

## Fixed this audit (2026-07-09)

| Finding | Severity | Fix |
|---|---|---|
| Same-process dual-open of served file → silent lost writes | **Critical** (data loss) | Host-instance sharing, ext-cpp `dc10558`; re-proven with persisted POSTs |
| DDL apply reloads dropped all DI attachments (loader dual-brain) | High (teardown hooks silently skipped) | Single loader path, `dc10558` |
| Inline session verify skipped server-side `expires_at` | Medium (soft-revoke bypass until cookie exp) | framework.sql `2e4d275`, tier-1 192/192 |
| Failed registry reload bricked server (`g_rt=NULL`) until next DDL | Medium (availability) | Swap-on-success in `quack_load_all_registries`, `dc10558` |
| Auth `kind` stored uppercase by sugar, oracle selector compares lowercase | Low (oracle track missed sugar-created schemes) | Lowercased at insert, `3485f13` |

## Open items (known, deliberate, tracked)

- **Secrets at rest are plaintext** in `quackapi_session_stores.secret`,
  `quackapi_auth.config_json`, and `api_keys.key` (BACKLOG §3.12). Anyone with
  file access owns the app — which is also true of a `.env` file, but unlike a
  `.env` the secret rides along with `COPY DATABASE`/backups. Planned: `SECRET
  ENV 'VAR_NAME'` indirection so the DB stores only the env-var name.
- **JWT C path splices** `''`-escaped token/secret into the verify SQL instead of
  binding (predates the bind-everything convention; escaping is correct for
  DuckDB string literals, so this is hardening debt, not a known injection).
  Migrate to prepared binds like the session path.
- **CSRF token via form fields** (`csrf_token`/`_csrf`) is honored by the oracle
  but not yet by the C worker (header `X-CSRF-Token` only). SPA/fetch clients are
  unaffected; classic form posts against the compiled server need the header.
- **TLS is proxy-terminated in v1** (vendored mbedtls is crypto-only). Set
  `COOKIE_SECURE true` in production behind the proxy.
- **Expired session rows accumulate** (lazy expiry). Sweep with a lifecycle hook
  or cron: `DELETE FROM quackapi_sessions WHERE expires_at < now() - INTERVAL 7
  DAY`.
- **Host-sharing lifetime**: with the shared-instance topology the serving
  session must stay alive (`block_forever(0)`) — killing the session while worker
  threads hold connections is undefined. The recipe already does this.
- **Process-global server singleton**: one `serve_brain` per process; a second
  instance loading the extension shares the brain globals. Not a security issue,
  but don't serve two databases from one process.

## Standing rules for new code

- New credential type ⇒ verification lives in framework.sql as a macro; the C
  worker calls it with binds. Never a second implementation.
- New registry ⇒ its load goes in `quack_load_all_registries` and nowhere else.
- Any secret-vs-input comparison ⇒ `_constant_time_str_equals`, no exceptions.
- Untrusted value into SQL ⇒ prepared bind. `''`-escaping is legacy, not license.
