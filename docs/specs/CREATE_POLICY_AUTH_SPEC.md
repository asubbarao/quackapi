---
title: "CREATE AUTH + CREATE POLICY — authentication & authorization as SQL DDL"
subtitle: "Security you declare, verified by the database engine — leaning on DuckDB core, not rolling our own crypto"
author: quackapi
date: 2026-07-02
---

# CREATE AUTH + CREATE POLICY

**Status:** spec v0 (flagship of the `CREATE <server-primitive>` family). Grammar will be
refined by the in-flight warehouse-DDL research fleet (Postgres RLS, Hasura/PostgREST, Snowflake
authentication/network/row-access policies, ClickHouse row policies + quotas, Kong/Envoy). This
document establishes the thesis, the **verified** SQL mechanism, and the honest edges.

## Thesis

FastAPI expresses auth as import-time Python objects (`Depends(oauth2_scheme)`, `python-jose`,
hand-rolled dependency functions). quackapi expresses it as **live SQL DDL**, because in a
database:

1. **Authorization is naturally a predicate.** "Can this request do this?" is a WHERE clause over
   the request's verified claims. **DuckDB's query engine *is* the policy evaluator.**
2. **Authentication is naturally a verification query.** A signed JWT is verified with
   `crypto_hmac` + base64 + `json` — all in-engine. No `python-jose`, no external service.
3. **The key material is native.** DuckDB **already has `CREATE SECRET` as first-class DDL.** We
   reuse it verbatim for signing keys — we do not invent a key store.

**We are not building security primitives.** We rely on the DuckDB core team's `crypto` extension
(HMAC/SHA-2) and native `CREATE SECRET`. quackapi only *composes* them into DDL. This is a feature,
not a limitation: the crypto is audited upstream; we add no attack surface of our own.

## Verified mechanism (this is not hand-waving)

The full HS256 JWT verification chain runs in pure DuckDB SQL. Measured on the canonical jwt.io
token (secret `your-256-bit-secret`):

```sql
LOAD crypto;
WITH jwt AS (
  SELECT
    -- the "header.payload" signing input and the third segment (signature)
    'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2MjM5MDIyfQ' AS signing_input,
    'SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c' AS sig_b64url,
    'your-256-bit-secret' AS secret            -- in production: read from CREATE SECRET, never a literal
)
SELECT
  crypto_hmac('sha2-256', secret, signing_input)
    = from_base64(replace(replace(sig_b64url,'-','+'),'_','/')
                  || repeat('=', (4 - (length(sig_b64url) % 4)) % 4))   AS signature_valid  -- => true
FROM jwt;
```

`signature_valid => true`. The payload segment then decodes via `from_base64(...)::VARCHAR` to the
claims JSON, parsed with DuckDB's native `json` functions into a `MAP` the policy layer consumes.

**Signature comparison must be constant-time** to avoid timing oracles. `=` on a BLOB in DuckDB is
length-then-bytes; v1 ships a `crypto_` constant-time compare if the extension exposes one, else a
folded-XOR SQL helper. Flagged as a must-fix before "secure," not after.

## `CREATE AUTH` — authentication (who are you?)

Registers a verification scheme. On each request the server resolves the scheme, verifies the
credential, and exposes a **verified claims MAP** (`claims`) to policies and handlers. Verification
failure short-circuits to **401**.

```sql
-- JWT bearer (HS256), signed with a key held in a native DuckDB SECRET
CREATE SECRET jwt_signing (TYPE quackapi_key, SECRET 'your-256-bit-secret');

CREATE AUTH bearer AS JWT (
  ALGORITHM  'HS256',            -- v1: HS256 (symmetric). RS256/JWKS => v2 (see edges)
  SECRET     jwt_signing,        -- references CREATE SECRET, never an inline literal
  HEADER     'Authorization',    -- where the token rides; 'Bearer ' prefix stripped
  VERIFY_EXP true,               -- reject expired (exp claim vs now)
  LEEWAY     30                  -- clock-skew seconds
);

-- API key (constant-time compare against a keys table; the DB is the store)
CREATE AUTH apikey AS API_KEY (
  HEADER 'X-API-Key',
  TABLE  api_keys,              -- columns: key_hash, subject, scopes
  HASH   'sha2-256'             -- store hashes, never raw keys
);
```

Backing state is ordinary tables (`quackapi_auth`, `api_keys`) — **introspectable** (`SELECT * FROM
quackapi_auth`) and **runtime-mutable** (`CREATE AUTH` / `DROP AUTH` live), which import-time Python
decorators cannot be.

**Stateless JWT sidesteps edge #5.** The edge-ledger flags "open transaction / session across a
request" as a REAL limit of the one-shot dispatch model. Stateless bearer tokens need *no*
server-side session — the claims travel in the token — so this is a point *in quackapi's favor*, not
a gap. Revocation (the one thing stateless tokens can't do alone) is handled the DuckDB-native way: a
`revoked_jti` denylist table joined during verification.

## `CREATE POLICY` — authorization (are you allowed?)

Authorization as a SQL predicate over the request's verified `claims` and `request` context.
Semantics borrowed directly from **Postgres RLS** (the research fleet is confirming the exact
stacking rules):

```sql
-- default-deny an admin surface; allow only admins
CREATE POLICY admin_only ON 'POST /admin/*'
  AS RESTRICTIVE
  USING (claims['role'] = 'admin');

-- users may read only their own record
CREATE POLICY own_record ON 'GET /users/{id}'
  AS PERMISSIVE
  USING (claims['sub'] = request['path']['id']);

-- a write guard distinct from the read guard (RLS USING vs WITH CHECK)
CREATE POLICY tenant_writes ON 'POST /orders'
  AS RESTRICTIVE
  WITH CHECK (claims['tenant'] = request['body']['tenant_id']);
```

**Combination semantics (from Postgres RLS):**
- **PERMISSIVE** policies are OR-combined — *any* passing permissive grants access.
- **RESTRICTIVE** policies are AND-combined — *every* restrictive must pass.
- A route with a restrictive policy is **default-deny**: no permissive match ⇒ 403.
- **`USING`** filters reads (does the caller get in); **`WITH CHECK`** filters writes (is the caller
  allowed to create *this* value). Same read/write split RLS draws.

`claims` is the verified-JWT MAP; `request` is `{method, path{params}, query{}, body{}, headers{}}`.
A failed policy on an *authenticated* caller ⇒ **403**; a policy needing claims with *no* valid auth
⇒ **401**.

## Enforcement in the pipeline

`handle_request` gains two phases between route-match and handler-execution:

1. **authenticate** — resolve the route's `CREATE AUTH` scheme (if any), verify the credential
   (the SQL above), build `claims`. Failure ⇒ 401. This is the C-router's job on the hot path
   (mirrors the compiled router owning routing/validation); the SQL oracle mirrors it for parity.
2. **authorize** — gather policies whose pattern matches `'<method> <path>'`, evaluate their
   predicates against `claims` + `request`, apply permissive-OR / restrictive-AND. Failure ⇒ 403.

Both are pure functions of `(claims, request, policy_rows)` — evaluable in the SQL oracle *and*
mirrored in C++, so the existing parity harness covers them for free once cases are added.

### How `claims` reach a policy predicate (decided)

A `USING (...)` predicate is SQL text stored in a policy row; the request's verified `claims` and
`request` context must be visible to it at evaluation time. Claims travel as **per-request data**, never
as session/connection state.

**Rejected: session variables.** A `current_setting`-style session/connection variable (the model
PostgREST uses under Postgres RLS) is *unsuitable here*, and understanding why sharpens the design.
Postgres reaches for it only because an RLS predicate is attached to the table and evaluated deep inside
the planner, which has no channel to receive per-request parameters — a session GUC is the single crack
left open. **quackapi owns its own evaluation harness**, so it is not trapped by that constraint, and a
session-scoped variable would be actively wrong: the server multiplexes many in-flight requests across a
shared pool of worker connections, so a connection-scoped "current claims" races between requests. We do
not use it.

**C++ server (hot path) — bound parameters.** Each policy compiles once to an evaluation statement of the
shape

```sql
SELECT (<predicate>) FROM (SELECT $1::json::map AS claims, $2::json AS request)
```

The worker binds *this* request's `claims` / `request` JSON as `$1` / `$2` on its own connection and
executes. `claims` and `request` are ordinary columns in scope, so the stored predicate reads them
directly. This is race-free (parameters are per-request, never shared state), fast (prepared once per
policy), and injection-safe (request data is bound, only the admin-authored predicate text is inlined).

**Pure-SQL oracle (parity tier).** A SQL macro cannot `EXECUTE` a stored predicate string, so the oracle
evaluates it by templating `claims` + `request` in and running it through the framework's **internal
execution channel** — the same mechanism the pure tier uses to run any dynamically-generated SQL. It is
race-free because each evaluation is its own connection/transaction.

Net: no session variables anywhere; the C tier binds parameters, the pure tier uses its internal
execution channel, and both evaluate the identical predicate so the parity harness still covers them as
one.

## Why this leans on DuckDB core maximally

| Primitive | DuckDB core we reuse | We do NOT build |
|---|---|---|
| Signing key storage | native `CREATE SECRET` | a key vault |
| JWT signature verify | `crypto` ext `crypto_hmac('sha2-256', …)` | any crypto |
| Claims parsing | native `json` / `from_base64` | a JWT parser lib |
| Policy evaluation | the query engine (predicate over a MAP) | a policy engine |
| Introspection | `SELECT * FROM` the policy/auth tables | a config format |

## Honest edges (name them, don't fake them)

1. **RS256 / JWKS (asymmetric).** `crypto_hmac` is symmetric (HS256). RS256 needs RSA-verify against
   a public key / JWKS endpoint. **v1 = HS256 only**; RS256 is v2, gated on whether the `crypto`
   extension exposes RSA verification (to confirm — do not assume).
2. **OAuth2 / OIDC authorization-code flow.** quackapi **verifies** tokens; it does not run the IdP
   redirect/state/token-exchange dance. Pair it with an external IdP (Auth0/Cognito/Keycloak) that
   mints the JWT; quackapi consumes it. Documented boundary, not a hidden gap.
3. **Constant-time comparison.** Must land before any "secure" claim (see above).
4. **Token revocation** on stateless JWT requires the `revoked_jti` denylist join — real, small,
   DuckDB-native.
5. **Policy predicate sandboxing.** `USING (...)` is arbitrary SQL evaluated per request. It must be
   restricted to pure expressions over `claims`/`request` (no subqueries against secret tables, no
   side effects) — a validation pass at `CREATE POLICY` time, mirrored in C++.

## Effort

- `CREATE AUTH (JWT HS256)` + `CREATE SECRET` reuse: **M** — verification SQL proven; work is the DDL
  parser hook (ParserExtension, same path as `CREATE ROUTE`) + the C-router authenticate phase.
- `CREATE POLICY` + RLS-style stacking: **M** — predicate storage + evaluation; the SQL is a WHERE
  clause, the work is the grammar + combination semantics + sandboxing pass.
- `CREATE AUTH (API_KEY)`: **S**.

## Open (pending research-fleet return)

- Exact `USING`/`WITH CHECK` combination edge cases (Postgres agent).
- ~~Claims-injection ergonomics~~ — **DECIDED** (see "How `claims` reach a policy predicate" above):
  bound parameters on the C hot path, the internal execution channel in the pure tier, **no session
  variables**. The PostgREST `current_setting` model is explicitly rejected — it's a workaround for a
  Postgres planner constraint quackapi does not share, and would race across the shared worker pool.
- Whether to adopt Snowflake's **NETWORK POLICY** (IP allow/deny) and **SESSION POLICY** ideas as
  additional `CREATE POLICY` kinds (Snowflake agent).
- Rate-limit / CORS grammars land in their own specs (gateway agent).
