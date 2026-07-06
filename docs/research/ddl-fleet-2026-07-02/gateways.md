# API Gateway Prior Art — quackapi DDL Design Reference

Research date: 2026-07-02  
Sources: Kong (developer.konghq.com), Envoy (envoyproxy.io), Traefik (doc.traefik.io), Trino (trino.io)

---

## 1. Kong Rate-Limiting / Rate-Limiting-Advanced

### Concept
Kong exposes rate limiting as a plugin applied to a service, route, consumer, or consumer-group.  
The advanced plugin (enterprise-ish) adds sliding-window, dual-window stacking, and Redis + cluster strategies.

### Config shape (Rate-Limiting-Advanced)

```json
{
  "config": {
    "window_type": "sliding",       // "fixed" | "sliding"
    "limit":       [10, 100],       // parallel window limits
    "window_size": [60, 3600],      // parallel window sizes in seconds
    "strategy":    "redis",         // "local" | "cluster" | "redis"
    "sync_rate":   5,               // seconds between async syncs; -1 = synchronous
    "namespace":   "my_ns",         // logical counter namespace
    "identifier":  "consumer",      // consumer | credential | ip | service | header | path | consumer-group
    "header_name": null,            // when identifier == "header"
    "path":        null,            // when identifier == "path"
    "hide_client_headers": false,
    "fault_tolerant": true,         // proxy through if data store unavailable
    "error_code":  429,
    "error_message": "API rate limit exceeded",
    "redis": {
      "host": "redis.example.com",
      "port": 6379,
      "password": "secret",
      "ssl": true
    }
  }
}
```

### Sliding window algorithm (Kong's implementation)
The sliding window counter weights the previous window proportionally:

```
effective_count = prev_window_count * (1 - elapsed_fraction) + current_window_count
```

If a 60s window is 25% through, previous window contributes 75%.  
This avoids the fixed-window burst problem (double-rate at boundary) without the O(n) memory cost of the sliding log.

### Key ideas to steal
- **Dual-window stacking**: `limit=[10,1000], window_size=[60,3600]` — single rule enforces both rpm AND rph simultaneously.
- **`identifier` enum** cleanly separates WHAT you're measuring from HOW you limit it.
- **`sync_rate`**: decouple counter accuracy from latency.
- **`fault_tolerant`**: explicit degraded-mode behavior.

---

## 2. Envoy Rate Limit Filter + envoyproxy/ratelimit service

### Concept
Envoy splits rate limiting into two layers: the sidecar filter builds **descriptor tuples** from request attributes; the external ratelimit service owns the limit definitions and Redis counters.

### Envoy filter — descriptor actions (virtual host / route config)

```yaml
rate_limits:
  - actions:
    - generic_key:
        descriptor_value: my_service
    - remote_address: {}          # client IP from x-forwarded-for
  - actions:
    - header_value_match:
        descriptor_key: PATH
        descriptor_value: api
        headers:
          - name: ":path"
            safe_regex_match:
              regex: "/api/v[0-9]+/.*"
    - request_headers:
        header_name: "X-API-Key"
        descriptor_key: "api_key"
```

Each `actions` block produces one descriptor tuple sent to the ratelimit service via gRPC.

### ratelimit service — limit definitions (YAML)

```yaml
domain: my_service
descriptors:
  - key: generic_key
    value: my_service
    descriptors:
      - key: remote_address
        rate_limit:
          unit: minute
          requests_per_unit: 100
  - key: PATH
    value: api
    descriptors:
      - key: api_key
        rate_limit:
          unit: second
          requests_per_unit: 10
  # wildcard + shadow mode
  - key: PATH
    value: /admin/*
    rate_limit:
      unit: second
      requests_per_unit: 5
      shadow_mode: true   # observe-only, always returns OK
```

### Key ideas to steal
- **Descriptor tuple composition**: rate limit key = AND of multiple attributes. Enables "per-IP per-path" rules without combinatorial config explosion.
- **Shadow mode**: enforce a limit in read-only mode (log over-limit but don't block). Perfect for dry-run / canary.
- **`replaces`**: a more specific rule overrides a less specific one without duplication.
- **External service separation**: limits live in the ratelimit service, not Envoy config — allows hot-reload without restarting the proxy.

---

## 3. Traefik Rate Limit Middleware

### Concept
Token bucket with configurable source criterion. Simpler than Kong/Envoy; no sliding window in open-source edition.

### Config shape

```yaml
http:
  middlewares:
    api-ratelimit:
      rateLimit:
        average: 100          # tokens refilled per period
        period: 1s            # refill period (token bucket)
        burst: 200            # max bucket depth (burst capacity)
        sourceCriterion:
          ipStrategy:
            depth: 1          # X-Forwarded-For position from right
          requestHeaderName: ""
          requestHost: false
        redis:
          endpoints:
            - "redis:6379"
```

### Sliding window note
Traefik uses **token bucket** (not sliding window). `average` = fill rate, `burst` = bucket depth.  
Token bucket allows instantaneous bursts up to `burst` then throttles at `average/period`.

### Key ideas to steal
- **`burst` / `average` separation**: explicit distinction between sustained rate and burst capacity. Much cleaner than just "N requests per window."
- **`sourceCriterion`** is a single discriminator object — exactly one of ipStrategy / requestHeaderName / requestHost may be set. This is a cleaner enum-as-struct than Kong's flat `identifier` string.
- **`period` as a first-class param** (not just a fixed unit enum) allows fractional-second windows.

---

## 4. Kong CORS Plugin

### Config shape

```json
{
  "config": {
    "origins":             ["https://example.com", "https://*.example.com"],
    "methods":             ["GET", "POST", "PUT", "DELETE", "OPTIONS"],
    "headers":             ["Content-Type", "Authorization"],
    "exposed_headers":     ["X-Custom-Header"],
    "credentials":         true,
    "max_age":             3600,
    "preflight_continue":  false,
    "private_network":     false,
    "allow_origin_absent": true
  }
}
```

### Key ideas to steal
- **`preflight_continue`**: proxy OPTIONS to upstream instead of short-circuiting. Allows upstream CORS negotiation.
- **`private_network`**: maps to `Access-Control-Allow-Private-Network` (new W3C spec) — forward-looking.
- **`allow_origin_absent`**: skip CORS headers when `Origin` is missing (non-browser requests). Prevents leaking CORS headers to non-CORS clients.
- **`exposed_headers`** vs **`headers`**: separate `Allow-Headers` (what the browser may send) from `Expose-Headers` (what JS may read from the response).

---

## 5. Envoy JWT Authn Filter

### Config shape

```yaml
http_filters:
  - name: envoy.filters.http.jwt_authn
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.jwt_authn.v3.JwtAuthentication
      providers:
        provider1:
          issuer: https://auth.example.com
          audiences:
            - api.example.com
          remote_jwks:
            http_uri:
              uri: https://auth.example.com/.well-known/jwks.json
              cluster: jwks_cluster
              timeout: 1s
            cache_duration: 300s
          forward: false                       # strip JWT before forwarding upstream
          forward_payload_header: X-JWT-Payload
          claim_to_headers:
            - header_name: X-User-ID
              claim_name: sub
            - header_name: X-User-Role
              claim_name: role
          from_headers:
            - name: Authorization
              value_prefix: "Bearer "
          from_params:
            - access_token
      rules:
        - match:
            prefix: /api
          requires:
            provider_name: provider1
        - match:
            prefix: /public
          requires: {}                         # skip JWT check
```

### Multiple providers with OR requirement

```yaml
rules:
  - match:
      prefix: /any
    requires:
      requires_any:
        requirements:
          - provider_name: internal_auth
          - provider_name: external_auth
```

### Key ideas to steal
- **`claim_to_headers`**: forward verified claims as request headers to the upstream. Lets the route handler trust `X-User-ID` without re-parsing the JWT.
- **`forward: false`**: strip the raw token before hitting upstream (no credential leakage).
- **`requires_any`**: accept tokens from multiple issuers on the same route (multi-tenant, federated identity).
- **JWKS cache**: `cache_duration` decouples key rotation from request latency.
- **`from_headers`, `from_params`, `from_cookies`**: token extraction is separately configured from verification — the what vs. the how.

---

## 6. Kong Key-Auth Plugin

### Config shape

```json
{
  "config": {
    "key_names":       ["apikey", "x-api-key"],
    "key_in_header":   true,
    "key_in_query":    true,
    "key_in_body":     false,
    "hide_credentials": true,
    "anonymous":       null
  }
}
```

### Key ideas to steal
- **`key_names` array**: allow multiple header/param names for backward compatibility during migration.
- **`hide_credentials`**: strip the key from the forwarded request — first-class security primitive.
- **`anonymous`**: fallback consumer identity for unauthenticated requests (opt-in pass-through). Enables mixed-auth routes.

---

## 7. Trino File-Based Access Control

### Config shape

```json
{
  "catalogs": [
    { "role": "admin",     "catalog": "(mysql|system)", "allow": "all" },
    { "catalog": "public", "allow": "read-only" }
  ],
  "schemas": [
    { "role": "admin",  "owner": true },
    { "user": "guest",  "owner": false }
  ],
  "tables": [
    {
      "schema": "default",
      "table": ".*",
      "privileges": ["SELECT"],
      "filter": "user = current_user",
      "columns": [
        { "name": "ssn",   "mask": "'XXX-XX-' || substring(ssn, -4)" },
        { "name": "email", "allow": false }
      ]
    }
  ],
  "queries": [
    { "role": "admin", "allow": ["execute", "kill", "view"] },
    {                  "allow": ["execute"] }
  ]
}
```

### Key ideas to steal
- **Column masks**: `"mask": "'XXX-XX-' || substring(ssn, -4)"` — inline SQL expression transforms the column before returning data. A first-class row/column security primitive.
- **`filter` predicate**: row-level security as a SQL WHERE clause appended to every query. Trino evaluates it as `current_user`-aware. Maps cleanly to a DuckDB view or macro.
- **First-match-wins ordered rules**: regex patterns on user/role/catalog/schema/table evaluated top-to-bottom. Simple, debuggable, composable.
- **Privilege separation**: `SELECT`, `INSERT`, `DELETE`, `UPDATE`, `OWNERSHIP`, `GRANT_SELECT` — not just allow/deny.

---

## Red-team: KEEP vs SKIP

| Idea | Source | Verdict | Rationale |
|------|--------|---------|-----------|
| Sliding window counter (weighted prev window) | Kong advanced | **KEEP** | Best accuracy/cost trade-off; maps to DuckDB window aggregate |
| Dual-window stacking (rpm + rph in one rule) | Kong advanced | **KEEP** | Trivially expressible as multiple LIMIT clauses in DDL |
| Token bucket burst/average | Traefik | **KEEP** | Burst capacity is a real need; complement to sliding window |
| Descriptor tuple composition (AND of attributes) | Envoy | **KEEP** | Very powerful; in quackapi maps to WHERE clause on rate_limit_log |
| Shadow mode | Envoy | **KEEP** | Essential for canary-deploys of new rate limits |
| `replaces` (specific overrides generic) | Envoy ratelimit | **SKIP** | Adds complexity; first-match-wins ordering achieves same result more simply |
| claim_to_headers mapping | Envoy JWT | **KEEP** | Upstreams should receive plain headers, not re-parse JWT |
| requires_any (multi-issuer OR) | Envoy JWT | **KEEP** | Multi-tenant is table stakes |
| JWKS remote + cache_duration | Envoy JWT | **KEEP** | Key rotation without restart |
| hide_credentials | Kong key-auth | **KEEP** | Strip token before forwarding; always on |
| anonymous fallback consumer | Kong key-auth | **SKIP** | Complicates auth model; handle via POLICY NOT AUTHENTICATED |
| Column masks as SQL expressions | Trino | **KEEP** | Native SQL; quackapi can inject as view wrapper |
| Row-level filter predicate | Trino | **KEEP** | Append to query WHERE; quackapi = macro wrapping SELECT |
| External ratelimit gRPC service | Envoy | **SKIP** | Overkill; DuckDB table + window agg replaces it |
| Redis synchronization | Kong/Traefik | **SKIP for now** | DuckDB-local state is fine for single-process quackapi; add as future extension |
| preflight_continue | Kong CORS | **SKIP** | quackapi handles OPTIONS itself; no upstream for OPTIONS |
| private_network CORS header | Kong CORS | **KEEP** | One boolean, future-proof |

---

## Proposed quackapi DDL Grammars

### CREATE RATE LIMIT

Informed by: Kong advanced (dual-window, identifier enum, fault_tolerant), Traefik (burst/average separation), Envoy (descriptor tuple AND composition, shadow mode).

```sql
-- Minimal: 100 rpm per IP, sliding window
CREATE RATE LIMIT api_standard
  WINDOW sliding 60 SECONDS LIMIT 100
  KEY ip;

-- Full form: dual-window, token bucket burst, shadow mode, custom response
CREATE RATE LIMIT api_premium
  -- Multiple WINDOW clauses = all must pass (Kong dual-window stacking)
  WINDOW sliding  60 SECONDS  LIMIT 200
  WINDOW sliding 3600 SECONDS LIMIT 2000
  -- Token bucket burst capacity (Traefik)
  BURST 50
  -- Key = what to partition by (Kong identifier / Traefik sourceCriterion)
  KEY consumer_id                      -- consumer | ip | api_key | header('X-Tenant')
  -- Descriptor tuple: AND of multiple attributes (Envoy)
  AND KEY header('X-Tenant-ID')
  -- Shadow mode: observe but don't block (Envoy)
  SHADOW
  -- Degraded mode behavior (Kong fault_tolerant)
  ON STORE UNAVAILABLE PASS
  -- Response
  ERROR CODE 429 MESSAGE 'Rate limit exceeded'
  -- Optional: expose X-RateLimit-* headers (default true)
  HEADERS VISIBLE;

-- Route-scoped application
ATTACH RATE LIMIT api_premium TO ROUTE /api/v2/**;
ATTACH RATE LIMIT api_standard TO ROUTE /public/** EXCEPT /public/health;
```

**DuckDB backing table:**
```sql
CREATE TABLE rate_limit_log (
  limit_name   VARCHAR,
  window_key   VARCHAR,              -- hash of KEY values
  window_start TIMESTAMPTZ,
  hit_count    INTEGER,
  PRIMARY KEY (limit_name, window_key, window_start)
);

-- Sliding window evaluation (inline):
-- effective = prev_hits * (1 - elapsed/window_secs) + current_hits
WITH windows AS (
  SELECT
    sum(hit_count) FILTER (WHERE window_start = date_trunc('minute', now() - interval '1 minute'))
      AS prev_count,
    sum(hit_count) FILTER (WHERE window_start = date_trunc('minute', now()))
      AS curr_count,
    extract(epoch FROM now() - date_trunc('minute', now())) / 60.0 AS elapsed_frac
  FROM rate_limit_log
  WHERE limit_name = 'api_premium' AND window_key = $1
)
SELECT prev_count * (1 - elapsed_frac) + curr_count AS effective_count FROM windows;
```

---

### CREATE CORS

Informed by: Kong CORS (all 9 parameters), Envoy (runtime-disable), Trino (first-match-wins ordering).

```sql
-- Minimal: public API, no credentials
CREATE CORS public_api
  ORIGINS *
  METHODS GET, POST, OPTIONS
  MAX AGE 86400;

-- Full form: credentialed, restricted origins, exposed headers
CREATE CORS storefront
  ORIGINS 'https://app.example.com', 'https://*.partner.io'
  METHODS GET, POST, PUT, DELETE, PATCH, OPTIONS
  ALLOW HEADERS 'Content-Type', 'Authorization', 'X-Request-ID'
  EXPOSE HEADERS 'X-Rate-Limit-Remaining', 'X-Request-ID'
  CREDENTIALS                          -- Access-Control-Allow-Credentials: true
  MAX AGE 3600                         -- preflight cache seconds
  PRIVATE NETWORK                      -- Access-Control-Allow-Private-Network: true
  SKIP ON ABSENT ORIGIN;               -- Kong allow_origin_absent: skip headers for non-browser

ATTACH CORS storefront TO ROUTE /api/**;
ATTACH CORS public_api  TO ROUTE /public/**;
```

**Notes:**
- `ORIGINS *` and `CREDENTIALS` together should raise a compile-time error (invalid combination per CORS spec).
- `ORIGINS` supports exact strings and glob patterns (`*.partner.io`); evaluated in order.
- `SKIP ON ABSENT ORIGIN` (default: true) avoids leaking CORS headers to non-browser clients.
- No `PREFLIGHT CONTINUE` — quackapi owns OPTIONS handling; upstream never sees it.

---

### CREATE AUTH (API Key + JWT)

Informed by: Kong key-auth (key_names, hide_credentials, key locations), Envoy JWT (providers, rules, claim_to_headers, requires_any, JWKS cache, forward: false), Kong JWT (claims_to_verify, maximum_expiration).

#### API Key flavor

```sql
-- Simple API key in header or query param
CREATE AUTH api_key_auth
  TYPE api_key
  -- Where to look (Kong key_names; multiple for migration compat)
  FROM HEADER 'X-API-Key', 'Authorization' PREFIX 'ApiKey '
  FROM QUERY PARAM 'api_key'
  -- Strip before forwarding (Kong hide_credentials; Envoy forward:false)
  STRIP CREDENTIAL
  -- On failure
  ERROR CODE 401 MESSAGE 'Invalid or missing API key'
  -- Expose identity downstream as header (Envoy claim_to_headers concept)
  FORWARD consumer_id AS HEADER 'X-Consumer-ID'
  FORWARD consumer_name AS HEADER 'X-Consumer-Name';

ATTACH AUTH api_key_auth TO ROUTE /api/**;
```

#### JWT flavor

```sql
-- Single-issuer JWT, remote JWKS
CREATE AUTH jwt_auth
  TYPE jwt
  -- Single provider (Envoy provider block)
  ISSUER 'https://auth.example.com'
  AUDIENCES 'api.example.com', 'mobile.example.com'
  JWKS REMOTE 'https://auth.example.com/.well-known/jwks.json'
    CACHE 300 SECONDS
    TIMEOUT 1 SECOND
  -- Validate standard claims (Kong claims_to_verify)
  VERIFY EXPIRY
  VERIFY NOT BEFORE
  MAX TOKEN AGE 86400 SECONDS          -- Kong maximum_expiration
  -- Where to find the token (Envoy from_headers / from_params / from_cookies)
  FROM HEADER 'Authorization' PREFIX 'Bearer '
  FROM QUERY PARAM 'access_token'
  FROM COOKIE 'jwt'
  -- Strip raw token from forwarded request (Envoy forward: false)
  STRIP CREDENTIAL
  -- Map claims to upstream headers (Envoy claim_to_headers)
  FORWARD CLAIM 'sub'   AS HEADER 'X-User-ID'
  FORWARD CLAIM 'email' AS HEADER 'X-User-Email'
  FORWARD CLAIM 'role'  AS HEADER 'X-User-Role'
  FORWARD CLAIM 'tid'   AS HEADER 'X-Tenant-ID'
  ERROR CODE 401 MESSAGE 'Invalid or expired token';

-- Multi-issuer OR (Envoy requires_any — multi-tenant / federated)
CREATE AUTH federated_auth
  TYPE jwt
  PROVIDER internal_auth
    ISSUER 'https://auth.example.com'
    JWKS REMOTE 'https://auth.example.com/.well-known/jwks.json' CACHE 300 SECONDS
  PROVIDER partner_auth
    ISSUER 'https://partner.identity.io'
    JWKS REMOTE 'https://partner.identity.io/.well-known/jwks.json' CACHE 600 SECONDS
  REQUIRE ANY PROVIDER                 -- Envoy requires_any
  STRIP CREDENTIAL
  FORWARD CLAIM 'sub' AS HEADER 'X-User-ID';

ATTACH AUTH jwt_auth TO ROUTE /api/** EXCEPT /api/public;
ATTACH AUTH federated_auth TO ROUTE /partner/**;
```

---

## Top-5 Gateway Ideas Worth Stealing (Ranked)

### 1. Sliding Window Counter (Kong)
The weighted-previous-window formula eliminates fixed-window boundary bursts while keeping O(1) state per key. In DuckDB this maps perfectly to a window aggregate over a 2-row time-bucket table. This is the correct default algorithm for `CREATE RATE LIMIT`. Fixed window should remain as an option (`WINDOW fixed`) but sliding is the sane default.

### 2. Descriptor Tuple Composition (Envoy)
Rate limit key = AND of multiple attributes. Instead of one-key-per-rule, compose: `KEY ip AND KEY header('X-Tenant-ID')` gives per-tenant-per-IP limits without n² rule explosion. In DuckDB this is a compound GROUP BY on the rate_limit_log table — completely natural.

### 3. claim_to_headers / FORWARD CLAIM (Envoy JWT)
After JWT verification, map named claims to upstream request headers. Upstreams never re-parse the JWT — they trust `X-User-ID`, `X-User-Role`. This is the most impactful usability improvement over raw JWT: the auth boundary is cleanly drawn at the gateway. Also forces you to think about what claims matter at design time, not route-handler time.

### 4. Shadow Mode (Envoy ratelimit)
`SHADOW` on a `CREATE RATE LIMIT` rule means: count hits, log over-limits, but always return OK. This is a first-class primitive for deploying new rate limits safely. Without it, every new limit is a potentially outage-causing change. With it, you observe real traffic against the proposed limit for a day before flipping `SHADOW OFF`.

### 5. Column Masks + Row Filters as SQL Expressions (Trino)
`"mask": "'XXX-XX-' || substring(ssn, -4)"` and `"filter": "user = current_user"` — access control expressed as SQL fragments. In quackapi this is the killer feature: a `CREATE POLICY` that wraps the target table in a DuckDB view injecting the filter/mask. Because quackapi is already inside DuckDB, there's no impedance mismatch — it's not a config DSL approximating SQL, it IS SQL.

---

## Algorithm Trade-off Summary for DuckDB Implementation

| Algorithm | Accuracy | Memory | Burst behavior | DuckDB mapping |
|-----------|----------|--------|----------------|----------------|
| Fixed window | Low (boundary burst) | O(1)/key | Allows 2x at boundary | date_trunc bucket + count |
| Sliding window counter | High | O(1)/key | Smooth | weighted 2-bucket agg |
| Sliding log | Perfect | O(n)/key | Exact | timestamp table + count(where > now-window) |
| Token bucket | Medium | O(1)/key | Explicit burst cap | last_refill + tokens column |

**Recommendation:** Default = sliding window counter. Expose `BURST N` to layer token-bucket-style burst cap on top of the sliding window sustained rate. This gives Kong-style accuracy with Traefik-style burst control.
