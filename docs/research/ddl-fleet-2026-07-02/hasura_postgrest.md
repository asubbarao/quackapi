# Hasura & PostgREST: Permission Model Prior Art for quackapi

Research date: 2026-07-02  
Purpose: Steal the best ideas for `CREATE POLICY` / `CREATE AUTH` DDL in quackapi — a FastAPI-equivalent built entirely inside DuckDB as live SQL DDL.

---

## Part 1: Hasura

### One-line concept
Hasura is a GraphQL gateway that reads Postgres schema at startup and provides **declarative, config-driven role × table × CRUD permission rules** stored as metadata (YAML or JSON), entirely outside SQL. The database never knows about Hasura roles.

### 1a. How permissions are expressed

Hasura permissions live in metadata files (applied via `hasura metadata apply` or the Metadata API). The unit of configuration is a **role × table × operation** triple:

```yaml
- table:
    schema: public
    name: products
  select_permissions:
    - role: user
      permission:
        columns:
          - id
          - name
          - price
        filter:
          price:
            _lt: 1000
  insert_permissions:
    - role: user
      permission:
        columns:
          - name
          - price
        check:
          owner_id:
            _eq: X-Hasura-User-Id   # session variable reference
        set:
          owner_id: X-Hasura-User-Id  # column preset — auto-inject claim on insert
```

Key structural points:
- `filter` = row-level predicate applied on **SELECT / UPDATE / DELETE** (WHERE equivalent)
- `check` = row-level predicate applied on **INSERT / UPDATE** (CHECK constraint equivalent — evaluated against the *resulting* row)
- `columns` = whitelist of columns this role can read/write for this operation
- `set` / column presets = fields that are force-set from session variables (user can't override)
- `aggregations` = flag that unlocks aggregate queries for a role

### 1b. Boolean predicate tree

Hasura's filter/check values are a recursive JSON tree:

```json
{
  "_and": [
    { "status": { "_eq": "published" } },
    { "_or": [
        { "owner_id": { "_eq": "X-Hasura-User-Id" } },
        { "is_public": { "_eq": true } }
    ]}
  ]
}
```

Operators: `_eq`, `_neq`, `_lt`, `_lte`, `_gt`, `_gte`, `_in`, `_nin`, `_is_null`, `_ilike`, `_like`, `_similar`, `_regex`.

Array session variables: if the JWT claim is `X-Hasura-Allowed-Ids: {1,2,3}`, use `_in: X-Hasura-Allowed-Ids`.

### 1c. `_exists` — cross-table predicate

Authorizes based on a row existing in an *unrelated* table:

```json
{
  "_exists": {
    "_table": { "schema": "public", "name": "users" },
    "_where": {
      "_and": [
        { "id": { "_eq": "X-Hasura-User-Id" } },
        { "allow_product_create": { "_eq": true } }
      ]
    }
  }
}
```

This is a correlated subquery expressed as config — no SQL written by the developer.

### 1d. Relationship-based permissions

Hasura can traverse declared table relationships inside permission predicates:

```json
{
  "usersInVendorsByVendorId": {
    "user_id": { "_eq": "X-Hasura-User-Id" }
  }
}
```

This generates a SQL EXISTS subquery over the join defined in the relationship metadata.

### 1e. JWT → session variables injection

**JWT structure Hasura expects:**
```json
{
  "sub": "1234",
  "iat": 1700000000,
  "https://hasura.io/jwt/claims": {
    "x-hasura-allowed-roles": ["user", "editor"],
    "x-hasura-default-role": "user",
    "x-hasura-user-id": "42",
    "x-hasura-vendor-id": "acme-corp"
  }
}
```

Mapping:
1. Hasura verifies the JWT (HMAC or RSA, configured at server startup).
2. It extracts all `x-hasura-*` keys from the claim namespace as **session variables**.
3. Every `x-hasura-*` key becomes a named session variable usable in permission predicates by its uppercased form: `x-hasura-user-id` → `X-Hasura-User-Id`.
4. The role is determined by the `X-Hasura-Role` request header (if present) or falls back to `x-hasura-default-role` from the token.
5. `claims_map` config can remap arbitrary JWT paths to session variables:  
   `"x-hasura-user-id": { "path": "$.user.id" }` — extracts from nested JWT structure.

**Critical design point**: session variables are interpolated as *literals* into the SQL predicate at query construction time — they are NOT function calls inside SQL. The predicate `{ "owner_id": { "_eq": "X-Hasura-User-Id" } }` becomes `WHERE owner_id = '42'` after session variable substitution, before the query hits Postgres.

### 1f. KEEP vs SKIP (Hasura)

| Concept | KEEP / SKIP | Note |
|---|---|---|
| role × table × CRUD permission triple | **KEEP** | Clean unit of authorization |
| filter (SELECT) vs check (INSERT result) distinction | **KEEP** | Not the same predicate — important |
| Column whitelist per operation | **KEEP** | quackapi routes expose columns |
| Column presets (force-set from claim) | **KEEP** | Prevents privilege escalation |
| Boolean predicate tree (`_and`/`_or`) | **KEEP** | But express in SQL not JSON |
| `_exists` cross-table sub-predicate | **KEEP** → express as SQL subquery in USING clause | |
| Session variable substitution at plan time | **KEEP** → adapt: use DuckDB function for claim access | |
| Relationship traversal in permissions | **SKIP** | quackapi has no relationship metadata; SQL subquery is sufficient |
| YAML/JSON config stored outside DB | **SKIP** | quackapi is DDL-in-DuckDB; keep everything as SQL objects |
| GraphQL-specific aggregations permission | **SKIP** | Not applicable |

---

## Part 2: PostgREST

### One-line concept
PostgREST is a REST gateway that **delegates 100% of authorization to PostgreSQL RLS** — it verifies the JWT, switches the DB role, injects claims as session GUCs, then lets Postgres do all the work. No permission config in PostgREST itself.

### 2a. How permissions are expressed

PostgREST permissions live entirely in **SQL** — Postgres roles + RLS policies:

```sql
-- 1. Role scaffold
CREATE ROLE authenticator LOGIN NOINHERIT;
CREATE ROLE anon NOLOGIN;
CREATE ROLE webuser NOLOGIN;
GRANT anon TO authenticator;
GRANT webuser TO authenticator;

-- 2. Schema grants
GRANT USAGE ON SCHEMA api TO webuser, anon;
GRANT SELECT, INSERT ON api.posts TO webuser;
GRANT SELECT ON api.posts TO anon;  -- public read, no write

-- 3. Enable RLS
ALTER TABLE api.posts ENABLE ROW LEVEL SECURITY;

-- 4. RLS policies using JWT claims
CREATE POLICY own_posts ON api.posts
  AS PERMISSIVE
  FOR ALL
  TO webuser
  USING (
    author_id = (current_setting('request.jwt.claims', true)::json->>'sub')::bigint
  )
  WITH CHECK (
    author_id = (current_setting('request.jwt.claims', true)::json->>'sub')::bigint
  );

CREATE POLICY public_read ON api.posts
  AS PERMISSIVE
  FOR SELECT
  TO anon
  USING (published = true);
```

### 2b. JWT → claims injection mechanism

PostgREST's full flow per request:

1. Parse `Authorization: Bearer <JWT>` header.
2. Verify signature (HS256 or RS256, configured in `postgrest.conf`).
3. Extract role claim using `jwt-role-claim-key` (JSONPath, default `$.role`).
4. Execute `SET LOCAL ROLE <extracted_role>` — switches DB role for this transaction.
5. Serialize **all** JWT claims as JSON and execute:
   ```sql
   SET LOCAL request.jwt.claims = '{"sub":"42","role":"webuser","email":"foo@bar.com"}';
   ```
6. Also sets `request.headers`, `request.cookies`, `request.path`, `request.method` as session GUCs.
7. Run the query — RLS policies activate automatically for the role.
8. All `current_setting('request.jwt.claims', ...)` calls inside RLS policies see the claims.

**Access patterns inside RLS:**
```sql
-- Full claims object
current_setting('request.jwt.claims', true)::json

-- Single claim (Postgres 14+)
current_setting('request.jwt.claims', true)::json->>'sub'

-- Single claim (Postgres < 14, PostgREST also sets per-claim keys)
current_setting('request.jwt.claim.sub', true)

-- Role
current_user  -- the impersonated DB role, e.g. 'webuser'
```

### 2c. Pre-request hook

```sql
CREATE OR REPLACE FUNCTION public.check_user() RETURNS void AS $$
DECLARE
  email text := current_setting('request.jwt.claims', true)::json->>'email';
BEGIN
  IF email = 'banned@evil.com' THEN
    RAISE EXCEPTION 'Access denied'
      USING HINT = 'Your account has been suspended';
  END IF;
END
$$ LANGUAGE plpgsql;
```

Configure: `db-pre-request = "public.check_user"` in `postgrest.conf`.

Runs AFTER role switch, BEFORE main query. Can read all `request.*` GUCs and raise to abort.

### 2d. `db-anon-role` — zero-auth default

When no JWT is present, PostgREST switches to the configured `db-anon-role`. This role's RLS policies define what unauthenticated users can see. Clean separation: no "if authenticated" branching in policies, just per-role policies.

### 2e. SECURITY DEFINER functions / views

PostgREST views behave as `SECURITY DEFINER` (run as view owner's role, bypassing caller's RLS). For Postgres 15+, `security_invoker = true` on views restores RLS compliance. Functions can be `SECURITY DEFINER` to access private schemas, acting as safe elevation points.

### 2f. KEEP vs SKIP (PostgREST)

| Concept | KEEP / SKIP | Note |
|---|---|---|
| `SET LOCAL` session GUC for JWT claims | **KEEP** → map to DuckDB equivalent | Core mechanism; quackapi needs same |
| `current_setting('request.jwt.claims')` in predicates | **KEEP** → adapt to DuckDB scalar function | |
| Role = DB role (PostgreSQL native) | **PARTIAL SKIP** → quackapi roles are SQL objects but DuckDB has no SET ROLE | |
| `db-anon-role` pattern for no-auth default | **KEEP** → `CREATE AUTH ... DEFAULT ROLE anon` | |
| RLS USING vs WITH CHECK distinction | **KEEP** | Same as Hasura filter vs check |
| Pre-request hook function | **KEEP** → quackapi can have `CREATE AUTH ... HOOK my_fn()` | |
| All authz in DB, none in gateway | **KEEP** — but quackapi IS the DB, so this collapses | |
| Per-claim GUC (`request.jwt.claim.sub`) | **KEEP** → DuckDB `getvariable()` equivalent | |
| `SECURITY DEFINER` view workaround | **SKIP** | DuckDB views don't have role context |

---

## Part 3: Synthesis — What quackapi Should Steal

### The claims-injection question answered

Both systems converge on the same mechanism:
- **Hasura**: substitutes claim values as literals into predicates at plan-construction time (outside SQL)
- **PostgREST**: injects claims as session GUCs (`SET LOCAL`), then SQL predicates call `current_setting()` to retrieve them

For quackapi, **PostgREST's GUC approach is the right model** because quackapi IS the database — there is no "outside SQL" layer. The cleanest adaptation:

After JWT verification, store claims in DuckDB session state using `SET VARIABLE`:
```sql
SET VARIABLE _jwt_sub = '42';
SET VARIABLE _jwt_role = 'user';
SET VARIABLE _jwt_claims = '{"sub":"42","email":"foo@bar.com","role":"user"}';
```

Then `CREATE POLICY` predicates access these via `getvariable()`:
```sql
CREATE POLICY own_rows ON orders
  FOR SELECT
  ROLE user
  USING (owner_id = getvariable('_jwt_sub')::bigint);
```

Or with a thin helper macro:
```sql
-- Helper to avoid the boilerplate cast
CREATE MACRO jwt_claim(key) AS (
  json_extract_string(getvariable('_jwt_claims'), '$.' || key)
);

CREATE POLICY own_rows ON orders
  FOR SELECT
  ROLE user
  USING (owner_id = jwt_claim('sub')::bigint);
```

This mirrors `current_setting('request.jwt.claims')::json->>'sub'` in PostgREST, but expressed in DuckDB idiom.

**Why not Hasura's literal-substitution approach?** Because in quackapi, policies ARE SQL — you'd need a template engine to substitute claim values before executing the USING clause. GUCs (session variables) are the native SQL mechanism for this; they keep policies as static, parseable SQL objects.

---

## Top 5 Ideas to Steal

### #1 — filter vs check distinction (Hasura + PostgREST)
**What**: `USING` clause applies on READ (what rows you can see); `WITH CHECK` applies on WRITE (what rows you can create/modify). They are separate predicates.
**Why it's good**: Prevents the classic bug where a user can insert a row they can't read, or read rows they shouldn't have been able to create.
**quackapi DDL**:
```sql
CREATE POLICY own_orders ON orders
  FOR ALL
  ROLE user
  USING     (owner_id = jwt_claim('sub')::bigint)   -- read gate
  WITH CHECK (owner_id = jwt_claim('sub')::bigint);  -- write gate
```

### #2 — column whitelist per role × operation (Hasura)
**What**: A role can SELECT a table but only see a subset of columns; a different subset for INSERT.
**Why it's good**: Avoids shadow-hiding sensitive fields with separate views per role.
**quackapi DDL**:
```sql
CREATE POLICY own_profile ON users
  FOR SELECT
  ROLE user
  COLUMNS (id, email, display_name)           -- NOT password_hash, admin_notes
  USING (id = jwt_claim('sub')::bigint);
```

### #3 — column presets / force-set from claim (Hasura)
**What**: On INSERT/UPDATE, certain columns are automatically set from JWT claims and the user cannot override them.
**Why it's good**: Prevents privilege escalation (user can't forge their own `owner_id`).
**quackapi DDL**:
```sql
CREATE POLICY create_post ON posts
  FOR INSERT
  ROLE user
  SET (author_id = jwt_claim('sub')::bigint)  -- injected; user input for this col ignored
  COLUMNS (title, body);
```

### #4 — anon-role default with explicit escalation (PostgREST)
**What**: Unauthenticated requests always get a named role (`anon`) whose policies define exactly what they can see. No "if authenticated" branching — everything is per-role policies.
**Why it's good**: Zero-auth state is explicit and testable, not "fall through to no policy".
**quackapi DDL**:
```sql
CREATE AUTH jwt
  SECRET 'my-secret'
  ALGORITHM HS256
  DEFAULT ROLE anon              -- unauthenticated requests get this role
  CLAIM role AS _jwt_role        -- extract role from JWT for role switching
  CLAIM sub  AS _jwt_sub
  CLAIM email AS _jwt_email;
```

### #5 — pre-request hook for imperative cross-cutting checks (PostgREST)
**What**: A SQL function runs after auth/role-switch but before any route handler. Can raise an exception to abort. Has access to all request context (claims, headers, path).
**Why it's good**: Handles things declarative predicates can't — rate limit checks, account suspension, audit logging, complex multi-table existence checks.
**quackapi DDL**:
```sql
CREATE AUTH jwt
  ...
  HOOK check_account_active();   -- called before every handler; RAISE aborts

CREATE MACRO check_account_active() AS TABLE (
  SELECT CASE
    WHEN (SELECT suspended FROM users WHERE id = jwt_claim('sub')::bigint)
    THEN error('Account suspended')
  END
);
```

---

## Recommended Claims-Injection Mechanism for quackapi

**Mechanism**: DuckDB session variables set at request ingress, accessed via `getvariable()` inside `CREATE POLICY ... USING/WITH CHECK` predicates.

**Flow**:
1. Request arrives at quackapi HTTP layer.
2. `CREATE AUTH` definition specifies which claims to extract and under what session-variable names.
3. After JWT verification, quackapi executes (internally):
   ```sql
   SET VARIABLE _jwt_claims = '<full claims JSON>';
   SET VARIABLE _jwt_sub = '<sub claim>';
   SET VARIABLE _jwt_role = '<role claim>';
   -- ... one SET VARIABLE per declared CLAIM binding
   ```
4. Policy `USING`/`WITH CHECK` predicates call `getvariable('_jwt_sub')` or the `jwt_claim()` macro.
5. The matched `CREATE ROUTE` handler executes; its result set is column-filtered per the policy's `COLUMNS` clause.

**Why this wins over alternatives**:
- Keeps policies as **static, parseable SQL text** (not templates with `{{claim}}` substitution).
- DuckDB `SET VARIABLE` + `getvariable()` is the native, already-supported equivalent of Postgres `SET LOCAL` + `current_setting()`.
- The `jwt_claim(key)` macro provides a single stable API surface — changing claim layout only requires updating the macro, not every policy.
- Session variables are transaction-scoped in DuckDB (or can be reset between requests), matching PostgREST's `SET LOCAL` isolation guarantee.

**Syntax recommendation for `CREATE AUTH`**:
```sql
CREATE AUTH jwt_auth
  TYPE JWT
  SECRET getvariable('JWT_SECRET')         -- or JWKS_URL for RS256
  ALGORITHM HS256
  DEFAULT ROLE anon
  CLAIMS (
    sub   AS _jwt_sub,
    email AS _jwt_email,
    role  AS _jwt_role,
    org   AS _jwt_org
  )
  HOOK validate_account();                 -- optional pre-request function
```

**Syntax recommendation for `CREATE POLICY`**:
```sql
CREATE POLICY tenant_isolation ON orders
  FOR ALL
  ROLE user
  USING     (org_id = getvariable('_jwt_org'))
  WITH CHECK (org_id = getvariable('_jwt_org'))
  COLUMNS (id, status, total, created_at);   -- hide internal fields
```

---

## What NOT to steal

1. **Hasura's metadata-as-YAML-outside-DB**: quackapi is DDL-first; externalizing config defeats the architecture.
2. **Hasura's relationship traversal in permission predicates**: SQL subqueries in `USING` clauses cover this with less magic.
3. **PostgREST's `SET ROLE` / DB-role-per-user model**: DuckDB doesn't have multi-role GRANT infrastructure; roles in quackapi should be a quackapi-layer concept, not DB roles.
4. **PostgREST's `SECURITY DEFINER` view pattern**: No DuckDB equivalent; solve with route-level policy bypass for admin routes instead.
5. **Hasura's GraphQL-specific features** (aggregations permission, `_count`, subscriptions): out of scope.
