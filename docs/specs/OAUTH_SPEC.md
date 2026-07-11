# OAuth2 Authorization-Code + OIDC — spec (FastAPI #335, 57👍 · #1428, 37👍)

**Status:** DESIGN LOCKED 2026-07-10 · builds Wave-2 of CREATE_POLICY_AUTH_SPEC
**Prereqs shipped:** sessions + CSRF (store/mint/verify), CREATE AUTH sugar, policies,
JWT HS256, `_create_session`, `_build_set_cookie_header`, subscription-runner precedent
for C-plumbs/oracle-owns orchestration.

## 1. Scope and ecosystem positioning

quackapi builds ONLY the web leg — the part the ecosystem explicitly lacks:

| Piece | Owner | Why |
|---|---|---|
| Authorization-code + PKCE browser flow (redirect, callback, state) | **quackapi (this spec)** | quack_oauth marks it "future"; its reserved design is a CLI localhost listener, not a web callback route |
| Cookie session mint after login | **quackapi** (existing `_create_session`) | quack_oauth is purely token-based, no cookies |
| OIDC discovery (`.well-known/openid-configuration`) | **quackapi (this spec)** | absent from quack_oauth |
| Incoming bearer RS256/JWKS verification | **delegate to `quack_oauth_check_token`** when loaded + configured | never a second verification implementation (SECURITY.md invariant 1). quackapi's own `_verify_jwt_hs256` remains for HS256 shared-secret schemes only |
| Token-endpoint POST machinery for M2M grants | **quack_oauth** (client_credentials/refresh/device) | already shipped there; quackapi does not reimplement M2M |

`https://duckdb.org/docs/current/quack/security` + quack_oauth = SQL-door security.
quackapi = app-door security, same doctrine (loopback default, proxy TLS, auth as SQL macros).

## 2. Outbound mechanism ruling (supersedes OUTBOUND_POST_PROBE.md verdict)

The probe favored http_client-with-binds; the standing ruling (Alok, 2026-07-10) is **no
http_client dependency** — curl via shellfs/read_text, the stack's soldered HTTP client.
(The http_client double-send bug is fixed upstream regardless: query-farm/httpclient#34.)

The probe's injection objection to shell is answered **without escaping**: untrusted bytes
(`code`, `state`, PKCE verifier) never touch the command line. Recipe:

1. `COPY (SELECT ...bound values... ) TO '<tmp>/qa_oauth_<uuid>.form'` — form body
   (`grant_type=authorization_code&code=...&code_verifier=...&redirect_uri=...`) written
   with values URL-encoded in SQL (`_urlencode` macro) from **bound** inputs.
2. `read_text('curl -sS -X POST --data @<that file> --url <TOKEN_URL from registry> |')`
   — command line contains only the server-generated file path and the developer-trusted
   registry URL. `CLIENT_SECRET` also goes in the body file, never the command line
   (`ps` would leak it).
3. Delete the form file after the exchange (shellfs `rm` in the same flow; best-effort).

Concurrency: shellfs degrades under c8 (~630ms/req, probe finding 3) — acceptable at
login frequency; documented limit; revisit only if a high-QPS outbound path appears.

## 3. DDL surface

```sql
CREATE AUTH google AS OAUTH2 (
  CLIENT_ID '...', CLIENT_SECRET '...',
  AUTH_URL 'https://accounts.google.com/o/oauth2/v2/auth',
  TOKEN_URL 'https://oauth2.googleapis.com/token',
  USERINFO_URL 'https://openidconnect.googleapis.com/v1/userinfo',
  REDIRECT_URI 'http://localhost:8080/auth/google/callback',
  SCOPES 'openid email profile',
  STORE 'sessions'            -- names an existing CREATE SESSION STORE
);

CREATE AUTH keycloak AS OIDC (
  DISCOVERY 'https://kc.example.com/realms/r/.well-known/openid-configuration',
  CLIENT_ID '...', CLIENT_SECRET '...',
  REDIRECT_URI '...', STORE 'sessions'
);
```

- Lands one `quackapi_auth` row, `kind='oauth2'`, config_json carrying all fields
  (kind lowercased at insert — the 3485f13 lesson).
- OIDC: DISCOVERY is fetched **at DDL time** (developer-trusted URL — splice-safe) via
  curl; `authorization_endpoint`/`token_endpoint`/`userinfo_endpoint`/`jwks_uri`/`issuer`
  fill config_json. Discovery failure = DDL error (fail loud at declare time, not at
  first login).
- Auto-registers two routes (same DDL action, like register_redirect):
  `GET /auth/<name>/login` kind=`oauth_login`, `GET /auth/<name>/callback`
  kind=`oauth_callback`, route_id `qa_oauth_<name>_login|callback`.
- `DROP AUTH <name>` removes the auth row AND both routes.
- Oracle mirror: `register_oauth(name, config_json)` (pure writer, tests).

## 4. Flow state table

```sql
CREATE TABLE quackapi_oauth_flows (
  state VARCHAR PRIMARY KEY,     -- server-minted uuid hex; client can never choose it
  auth_name VARCHAR,
  pkce_verifier VARCHAR,         -- 64-char hex, server-minted
  redirect_after VARCHAR,        -- optional ?next= (validated: must start with '/')
  created_at TIMESTAMP,
  redeemed_at TIMESTAMP          -- single-use: set on redeem; second use = 401
);
```

TTL 10 minutes (checked at redeem, lazy delete like sessions). `state` doubles as the
CSRF binding for the flow (RFC 6749 §10.12); PKCE S256 additionally binds the code.

## 5. Oracle macros (the one implementation; C plumbs, never re-implements)

- `_urlencode(s)` — percent-encode for query/form components (pure SQL over bytes).
- `_pkce_challenge(verifier)` — `b64url(sha256(verifier))`; **verified against the
  RFC 7636 test vector** (dBjftJeZ… → E9Melhoa…) in tier-1.
- `_oauth_begin(auth_name)` TABLE → one row `(state, pkce_verifier, location)`:
  builds `AUTH_URL?response_type=code&client_id=…&redirect_uri=…&scope=…&state=…&
  code_challenge=…&code_challenge_method=S256`. Caller (C worker / tier-1 harness)
  INSERTs the flow row and emits `302 Location:` + `Cache-Control: no-store`.
- `_oauth_flow_redeem(state)` TABLE → the flow row iff unexpired + unredeemed
  (bound state; caller marks `redeemed_at` in the same statement sequence).
- `_oauth_exchange_sql(auth_name)` → composed SQL text for the curl exchange
  (SUB2-style: tier-1 asserts composition byte-exactly; live cert runs it).
- `_oauth_token_ok(auth_name, token_json)` — validates the exchange response:
  `error` absent, `access_token` present; if the scheme has `jwks` config AND
  quack_oauth is loaded, delegates ID-token check to `quack_oauth_check_token`;
  otherwise trusts the direct-TLS channel (OIDC code-flow allowance) and parses
  claims from userinfo.
- `_oauth_callback_response(auth_name, claims_json)` TABLE → `(status, headers_json,
  body)`: mints session via `_create_session` machinery + store config, headers =
  `{"Set-Cookie": …, "Location": redirect_after|'/'}`, status 302.

Untrusted inputs throughout (`code`, `state`, `next`, token response, userinfo body):
**prepared binds only** (SECURITY.md invariant 3). Registry values (URLs, client id) are
developer-trusted DDL and may be composed.

## 6. C worker dispatch (mirror, no second brain)

Route kinds `oauth_login` / `oauth_callback` dispatch a fixed statement sequence on the
**writer** connection (auth must read current data — invariant 5), every statement an
oracle macro call with binds:

- login: `_oauth_begin` → INSERT flow row → 302.
- callback: parse `code`,`state` from query (existing param machinery) →
  `_oauth_flow_redeem` (miss ⇒ 401 `{"detail":"invalid or expired oauth state"}`)
  → mark redeemed → COPY form file → `read_text(curl …)` → `_oauth_token_ok`
  (fail ⇒ 401, body logged to flow row's last_error, never echoed to client)
  → userinfo curl (same recipe, Authorization header via `-H @file` config)
  → INSERT session row → 302 + Set-Cookie.

Errors: provider `error=access_denied` on callback ⇒ 401 with generic detail.
No token, no client_secret, no code ever appears in a response body, span, or log
(hash-prefix only, quack_oauth's redaction convention).

## 7. Tests

- tier-1 (pure SQL): RFC 7636 vector; `_urlencode` edge bytes (space, `&`, `=`, `+`,
  unicode); `_oauth_begin` URL composition exact; state single-use (second redeem
  returns empty); expiry (11-min-old row invisible); `next` validation rejects
  `https://evil` and `//evil`; exchange-SQL composition byte-exact; injection probe
  (`'); DROP TABLE …--` as code/state) proves binds.
- live cert (serve_brain + a local SQL-scripted fake IdP on a scratch port):
  full round-trip login → 302 → callback → cookie → authed request → CSRF enforced;
  replayed state → 401; tampered code → 401 (fake IdP rejects); expired flow → 401.
- against-real-provider smoke (manual, post-merge): Google + a local Keycloak if
  available; not gated in CI.

## 8. Honest edges (documented, deliberate)

- Token exchange serializes under concurrent logins (shellfs; probe finding 3).
- No refresh-token persistence in v1 — the session cookie is quackapi's credential;
  provider tokens are used once (exchange + userinfo) and discarded. Storing provider
  tokens for API-call-on-behalf-of is a separate feature (and where quack_oauth's
  refresh machinery would slot in).
- OIDC discovery at DDL time only — provider key/endpoint rotation requires re-running
  CREATE AUTH (or future `REFRESH AUTH` sugar).
- ID-token RS256 verification is delegated (quack_oauth) or skipped in favor of the
  direct-TLS userinfo call — quackapi never grows its own JWKS/RS256 implementation.
