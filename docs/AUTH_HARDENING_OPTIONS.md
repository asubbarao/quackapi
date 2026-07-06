# quackapi Auth + Hardening Options

**Research date:** 2026-07-03  
**Scope (read-only except this file):** quackapi repo at /Users/aloksubbarao/quackapi.  
**Constraint:** Compose with the DuckDB ecosystem; do not reinvent auth/crypto.  
**Current baseline (verified):**  
- `serve_brain(port, db_path)` / `quack_serve_brain*` (ext-cpp/src/quackapi_brain.cpp:1392 and serve_brain.sql) does raw POSIX `socket/bind/listen/accept` with `a.sin_addr.s_addr = 0` (INADDR_ANY). Bind-to-127.0.0.1-by-default lands separately; assume it does.  
- Routing/validation is `handle_request(method, path, headers_json, body)` (framework.sql). `param_schema` already supports `location='header'` (and 'cookie', 'path', 'query', 'body'); values are extracted and go through the same `required` + `TRY_CAST` + constraint pipeline that produces FastAPI-shaped 422s. See framework.sql:480 (param_values CTE) and 530 (validation_errors).  
- Middleware stub exists (`middleware` table + `apply_pre`; auth_gate kind produces 401 on missing/wrong scheme; see middleware.sql:100+). Not wired into the C hot path by default.  
- No built-in auth or TLS in serve_brain (COMMUNITY_EXT_PATH.md §7 explicitly calls for "default to localhost (or document loudly)" + "no built-in auth yet" notes in description.yml).  
- Handlers are pure SQL; any `LOAD`ed community extension (crypto, httpfs secrets, etc.) is already available inside route handlers (COMPOSABILITY.md).  

**Citations for local sources (direct reads):**  
- README.md (thesis, pillars, serve_brain role).  
- framework.sql:1-100 (header + tables), 100- (param_schema JSON seed), 170- (handle_request macro, header extraction at ~480, OpenAPI param emission including header at ~690).  
- ext-cpp/src/quackapi_brain.cpp:1398-1404 (bind: `a.sin_addr.s_addr=0`; serve_brain_impl).  
- docs/COMMUNITY_EXT_PATH.md §7 (security bar, httpserver precedent, localhost default recommendation).  
- serve_brain.sql (C source for the loop; same bind logic ported).  
- middleware.sql + di.sql (existing pre-auth and header-based DI patterns).  

External sources fetched via web tools below; cited inline.

## Evaluation of the Six Options

### 1. DuckDB core secrets manager (`CREATE SECRET` / `duckdb_secrets()`)

**Mechanism (from DuckDB docs):**  
Secrets are typed (`TYPE s3`, `TYPE http`, `TYPE quack`, etc.). Most types are registered by core extensions (httpfs registers `http`/`s3`/`gcs` etc.; quack registers `quack`). `CREATE [PERSISTENT] SECRET name (TYPE t, ...)`; listed via `FROM duckdb_secrets()` (sensitive fields redacted). Scoped by prefix. Providers (config, credential_chain). Persistent secrets live unencrypted in `~/.duckdb/stored_secrets`. Extensions can register new types (httpfs, aws, postgres, quack, etc. do exactly this). See https://duckdb.org/docs/current/configuration/secrets_manager.html and CREATE SECRET statement page.  

**What quackapi must add:**  
Nothing for storage: users can already `CREATE SECRET my_api (TYPE http, EXTRA_HTTP_HEADERS MAP{'X-API-Key':'sekret'});` or a custom table. For server-side enforcement inside serve_brain/hot path, quackapi would need a tiny hook: either (a) a new `param_schema` constraint that names a secret (e.g. `constraint_json` with `{"secret":"my_api"}` or special "location=header, compare_to_secret"), or (b) a built-in scalar `quack_check_secret(header_name, secret_name)` usable in a handler or middleware expression. The C layer already lowercases header keys and passes the full JSON; a SQL-side comparison (or a C fast-path that calls into the secrets manager) would suffice. No raw key literals in route rows.  

**Effort:** S (if we only document "put the key in a secret + compare in handler SQL") to M (add a first-class param constraint or a `check_header_against_secret` helper that the router can short-circuit to 401 before handler render).  

**Verdict:** Excellent composition point. Do not invent a new key store. A `TYPE http` (or a new lightweight `quack_api_key` if an extension can register one cleanly) + a comparison helper is the natural hardening of "per-route auth as data". Custom TYPE registration is possible but not required for v1 (http extra-headers or a plain table both work today). Redaction in `duckdb_secrets()` is a bonus for operators.

### 2. Per-route auth as data (X-API-Key via existing `param_schema location='header'`)

**Current exact mechanism (zero new code):**  
```sql
-- 1. Declare a required header param on the route (already works)
INSERT INTO param_schema VALUES
  ('my_route', 'x-api-key', 'header', 'string', true, NULL);

-- 2. In the handler SQL (or a wrapper macro), compare against a secret/table value
--    (example using a compose_secrets-style table or a CREATE SECRET value)
SELECT ... FROM ... WHERE {x_api_key} = (SELECT secret FROM my_keys WHERE name='api');
-- or (future tiny hook)
-- WHERE quack_header_matches_secret({x_api_key}, 'my_api_key_secret')
```
The validation pipeline already treats header params identically to query/path (422 on missing or wrong type). See framework.sql:490 (`json_extract_string(headers, '$.' || ps.name)`), 530 (required check → 'missing'), and OpenAPI emission that already includes `in: header`. C layer lowercases keys (quackapi_brain.cpp header parse).  

Middleware `auth_gate` (middleware.sql:110) already demonstrates a pre-phase 401 short-circuit for `Authorization: Bearer ...` using the same `headers` JSON.  

**What tiny extension hook would harden it:**  
A built-in comparison scalar (or a special `constraint_json` form) that does constant-time compare against a named secret from the secrets manager (or a table row), and that the router can turn into a 401 instead of 422. E.g. a param entry with `constraint_json: '{"compare_secret":"my_key"}'` or a dedicated `location='header_auth'` that the C router recognizes. Keeps everything as data; no new DDL yet.  

**Effort:** S (document the table+handler pattern today) / M (add the secret-aware comparison helper + 401 path in validation or middleware integration).  

**Verdict:** This is the "do now" primitive. quackapi already has the surface; it just needs a documented pattern + one small comparison primitive to stop people from writing `= 'hardcoded'` in handlers. Perfect composability.

### 3. httpserver community extension's auth approach

**What they shipped (fetched from https://github.com/Query-farm/httpserver and duckdb.org/community_extensions):**  
`httpserve_start(host, port, auth)` — third parameter is the auth config:  
- `''` (empty) → no auth.  
- `'user:pass'` → HTTP Basic Auth (checked against the literal).  
- `'supersecretkey'` (any other non-empty, non-`:` string) → requires `X-API-Key: supersecretkey` header; exact match.  

Examples in their README (web:1, web:0):  
```sql
SELECT httpserve_start('localhost', 9999, 'user:pass');
curl -u user:pass ...
SELECT httpserve_start('localhost', 9999, 'supersecretkey');
curl -H "X-API-Key: supersecretkey" ...
```
They also show using a `CREATE SECRET (TYPE HTTP, EXTRA_HTTP_HEADERS ...)` for client-side when calling back into an httpserver instance. No per-route; global for the serve call. Includes a play UI. httpserver is the category precedent cited in COMMUNITY_EXT_PATH.md §7.

**What is worth mirroring in serve_brain's signature:**  
`serve_brain(port, db_path, access_log := 0)` (and the ex variant) could grow an optional 4th/keyword param for a global "auth token" or "basic" string, implemented the same way (simple header or Authorization check in the C acceptor before even calling into SQL). Keep it minimal and global; route-level is better served by #2. Document the parallel: "like httpserver, pass a token string for X-API-Key behavior."

**Effort:** S (add the param + one strcmp in the request path + docs).  

**Verdict:** Mirror the simple global token/X-API-Key shape for consistency with the only other raw-HTTP server extension. Do not copy the Basic literal-in-arg style for anything beyond dev; encourage the data-driven per-route approach for real use.

### 4. quackscale (https://github.com/Query-farm/quackscale)

**Mechanism (fetched README + community listing):**  
QuackScale embeds libtailscale/tsnet inside DuckDB. `CALL tailscale_up(hostname, state_dir, ...)` joins the tailnet (supports Headscale via `control_url`). Then:  
- Server: `CALL quack_serve('quack:127.0.0.1:9494', ...); CALL tailscale_serve_local(port => 9494);`  
  (Quack binds loopback; tailscale_serve_local makes it reachable on the tailnet without a public listener.)  
- Clients: `ATTACH 'quack:100.x.x.x:9494' ...` (or MagicDNS) after their own `tailscale_up`; traffic is WireGuard-encrypted.  

Auth layers (from their AUTHENTICATION.md references and README):  
- Tailnet membership (device identity).  
- Tailscale/Headscale ACLs (which nodes may connect to which ports/services).  
- Optional shared `QUACK_TAILNET_TOKEN` (or per-quack token) for the Quack protocol itself.  
Two independent checks: "are you on my mesh + allowed by ACL?" then "do you present a valid quack token?" Nothing listens on 0.0.0.0 public internet.  

See also quack docs security page: quack itself defaults to localhost + random token + recommends reverse proxy for TLS.

**What our README/docs should recommend:**  
" For private fleet / zero-trust exposure: LOAD quackscale; CALL tailscale_up(...); bind serve_brain (or quack_serve) to 127.0.0.1 only; use `tailscale_serve_local` (or equivalent forwarding) + tailnet ACLs as the network/auth layer. Add a quack-style token or X-API-Key on top for the application. No public port, WireGuard everywhere. Pairing is exactly: localhost bind + tailscale exposure + ACLs."

**Effort for quackapi:** S (docs + example compose snippet). No code change needed in serve_brain.

**Verdict:** Strong recommendation for the "do-before-community-PR" hardening story. This is how the ecosystem already solves "serve without listening publicly."

### 5. quack_oauth (https://github.com/DataZooDE/quack-oauth)

**Assessment (fetched README + community listing):**  
quack_oauth provides OAuth 2.1 / OIDC (JWKS signature verification, RFC 7662 introspection, Google tokeninfo, GitHub check, provider presets for Keycloak/Entra/Google/etc.) **strictly for the DuckDB quack wire protocol** (the `quack:` / port 9494 client-server protocol). It replaces the stub `quack_check_token` / `quack_nop_authorization` callbacks via `SET quack_authentication_function = 'quack_oauth_check_token';` and a `CREATE SECRET (TYPE quack_oauth_server, ...)` holding issuer/jwks_uri/audience + optional policy_table for claims-driven SQL-row authorization (actions like Attach/Scan parsed by DuckDB's own parser).  

It has JWKS caching, introspection, audit, per-user policies, etc. — all wired into quack's connection/query hooks.  

**Reusable for raw HTTP bearer validation in serve_brain?**  
No direct machinery. The validation and policy live in the quack protocol callbacks; there is no exported "validate this bearer against this JWKS for an arbitrary HTTP request" scalar that serve_brain could call on a raw `Authorization: Bearer ...` header. The extension is purpose-built around quack session ids and the quack wire format.  

**Honest verdict:** The right story is exactly the one in the repo's own positioning: "if you need OIDC/JWTs at scale, serve over the quack protocol with quack_oauth instead of (or in addition to) raw HTTP via serve_brain." For raw HTTP users who want bearer tokens, the practical path is (a) the per-route header + secret/table compare (#2), (b) a thin middleware that calls out to an external IdP introspection endpoint via curl_httpfs (already the soldered client), or (c) put a real auth proxy (oauth2-proxy, etc.) in front. Do not try to reuse quack_oauth's internals for HTTP.

**Effort:** Zero in quackapi (just document the boundary). L if someone later wanted to factor a shared "jwt validate" scalar out of it.

### 6. TLS options (ranked)

**Ranked (from TLS_SPEC.md in-repo + quack security docs + httpserver patterns):**  

1. **Reverse proxy (nginx / Caddy) — documented pattern (recommended default for any public or multi-user exposure).**  
   - Caddy (or nginx) terminates TLS (easy certs, ACME, HTTP/2), forwards plain HTTP to `127.0.0.1:port` where serve_brain listens (after the localhost-default lands).  
   - Same story quack docs recommend (https://duckdb.org/docs/current/quack/setup/reverse_proxy.html has Caddy + nginx recipes). httpserver and quack users already do this.  
   - Zero code in quackapi; just docs + example `Caddyfile`.  
   - Effort: S (docs).  

2. **Tailscale serve (HTTPS termination on the tailnet).**  
   - `tailscale serve` (or quackscale's `tailscale_serve_local`) can terminate HTTPS for you on the tailnet side while the backend binds localhost. Encryption is WireGuard; you get certs "for free" inside the tailnet.  
   - Pairs perfectly with #4. Already how quackscale + quack recommend doing "HTTPS without managing certs."  
   - Effort: S (docs + quackscale example).  

3. **Build TLS into serve_brain (last resort).**  
   - DuckDB vendors mbedTLS (ext-cpp/duckdb/third_party/mbedtls; used for sha/hmac inside core). TLS_SPEC.md analyzes using it for a uvicorn-style `--ssl-*` surface.  
   - Big dep surface (handshake, cert loading, ALPN, etc.), threading fit with the 16-worker pool, maintenance on every DuckDB mbedtls update, platform quirks. quack explicitly says "the server does not use TLS itself" for localhost.  
   - Effort: L (full integration + tests + config surface). Only if someone needs a single-binary no-proxy HTTPS story.  

**Verdict order:** Proxy documented first; tailscale serve second (when you are already on a tailnet); embed only if forced.

## Ranked Recommendations

### Do now (S, no machinery changes)
- **Document the per-route header pattern (#2) + "use CREATE SECRET or a keys table".** Add a short section + worked example in README and a new "security" note in docs/. Show exact `param_schema` row + handler `WHERE {x_api_key} = (SELECT ...)` (or middleware auth_gate extension). Mention constant-time compare is a follow-up.  
- **Default localhost + loud security note.** Once the bind-default change lands, update launch/run docs and the community description.yml text to say "binds localhost by default; for remote use a proxy or tailscale; no built-in auth — compose with DuckDB secrets + header params or middleware." (Matches COMMUNITY_EXT_PATH §7 checklist item 7.)  
- **Mirror httpserver's global token shape (S).** Optional 4th arg or keyword to serve_brain for a simple X-API-Key global guard (exact string compare in C before SQL). Document parity.

### Do before community-PR (S/M)
- **Add a tiny secret-aware comparison hook.** Either a scalar `quack_secret_matches(header_value, secret_name)` or a `constraint_json` form the router understands that short-circuits to 401 with proper `WWW-Authenticate` or `{"detail":"..."}`. Wire the existing middleware auth_gate into the C hot path (or make `handle_request` call apply_pre when middleware rows exist).  
- **Write the hardening section + quackscale pairing in README/docs.** Explicit recipes:  
  - "Local/dev: localhost bind + X-API-Key header param."  
  - "Private fleet: quackscale + tailscale_up + localhost bind + tailscale_serve_local + tailnet ACLs + optional token."  
  - "Public: Caddy (or nginx) → 127.0.0.1:serve_brain (see quack reverse-proxy docs for template)."  
- **Update COMMUNITY_EXT_PATH.md notes and any description.yml draft** with the "no built-in auth, use ecosystem secrets/headers/proxy/tailscale" language.  
- **Add sqllogictest coverage for header params + 422/401 cases** (already required for CI).

### Later (M/L, only if demand)
- **Full `CREATE AUTH` / policy DDL** (the aspirational CREATE_POLICY_AUTH_SPEC.md). This is the "nice to have" layer on top of the primitives above; it can be pure-SQL + a tiny router hook once the secret comparison exists.  
- **Embedded TLS** (per TLS_SPEC.md) — only after proxy + tailscale patterns are exhausted and a real need for single-binary HTTPS appears. Leverage the vendored mbedtls but treat as high-cost.  
- **OIDC bearer validation for raw HTTP** — factor a reusable JWT/introspection scalar out of quack_oauth (or stand on curl_httpfs + a JWKS table) only if users insist on raw-HTTP + IdP tokens. Default answer remains "use the quack protocol + quack_oauth for that."

## Summary Table

| Option | Mechanism | quackapi delta | Effort | Priority |
|--------|-----------|----------------|--------|----------|
| 1. Secrets | CREATE SECRET + duckdb_secrets(); extensions register TYPEs | Tiny comparison helper / constraint | S/M | High (compose) |
| 2. Per-route data | param_schema location=header + handler compare | Doc + optional secret cmp builtin | S | Do-now |
| 3. httpserver | httpserve_start(..., 'token') → X-API-Key or Basic | Mirror global token arg | S | Do-now (parity) |
| 4. quackscale | tailscale_up + quack_serve(127) + tailscale_serve_local; WG+ACLs | Docs + example only | S | Pre-PR |
| 5. quack_oauth | OIDC/JWKS/introspect for quack wire protocol only | Docs (boundary) | S | Pre-PR (clarify) |
| 6. TLS | Proxy (1), tailscale serve (2), embed mbedTLS (3) | Docs (proxy + tailscale first) | S / L | Proxy now; embed never by default |

**Bottom line:** quackapi's job is to expose the existing DuckDB surfaces (header params, secrets, middleware SQL, community network extensions) cleanly and document the composition patterns. Anything beyond a tiny comparison helper and good defaults is scope creep.

**Sources cited (external, via tool results):**  
- DuckDB Secrets Manager: https://duckdb.org/docs/current/configuration/secrets_manager.html (web:9, full fetch).  
- httpserver auth: https://github.com/Query-farm/httpserver (README + examples; web:1, web:0, web:31).  
- quackscale: https://github.com/Query-farm/quackscale (README; web:24, full fetch).  
- quack_oauth: https://github.com/DataZooDE/quack-oauth (README + architecture; web:33).  
- quack security + reverse proxy: https://duckdb.org/docs/current/quack/security (full fetch).  
- httpserver community page and community-extensions precedent (web:0, web:2).  

All local file content was read directly with the read_file/grep tools; no other files were written or modified.