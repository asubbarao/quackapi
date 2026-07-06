# PostgreSQL RLS & Role Model — Ideas for quackapi `CREATE POLICY`

Research target: steal the best ideas from Postgres RLS for quackapi's `CREATE POLICY ON '<method> <path>'`
that expresses HTTP authorization as a SQL predicate over JWT claims.

---

## 1. `CREATE POLICY` — Full Grammar & Stacking Semantics

### Exact Grammar (Postgres 16+)

```sql
CREATE POLICY name ON table_name
    [ AS { PERMISSIVE | RESTRICTIVE } ]
    [ FOR { ALL | SELECT | INSERT | UPDATE | DELETE } ]
    [ TO { role_name | PUBLIC | CURRENT_ROLE | CURRENT_USER | SESSION_USER } [, ...] ]
    [ USING ( using_expression ) ]
    [ WITH CHECK ( check_expression ) ]
```

### Real Example

```sql
-- Permissive: any member of 'staff' or 'admin' role can read their own rows
CREATE POLICY staff_see_own ON timesheet
    TO staff
    USING (employee_id = current_user::integer);

-- Restrictive: even admins must connect from localhost
CREATE POLICY local_only ON timesheet AS RESTRICTIVE TO admin
    USING (pg_catalog.inet_client_addr() IS NULL);
```

### Concept

A named, per-table predicate that the SQL engine automatically ANDs or ORs into every query
touching that table, evaluated in the security context of the calling user.

### Why It's a Good Idea

- The authorization rule lives **in the schema**, not scattered across application layers
- Named policies are independently inspectable, testable, and auditable (`pg_policies` catalog)
- The dual PERMISSIVE/RESTRICTIVE axis is expressive enough to model both "allow any of these" and
  "always require this" without a Turing-complete policy language

### Mapping to quackapi

```sql
CREATE POLICY allow_own_docs ON 'GET /documents/:id'
    USING (claims->>'sub' = route_params->>'id');

-- RESTRICTIVE equivalent: hard gate that ALL policies must pass
CREATE POLICY require_tls ON 'GET /documents/:id' AS RESTRICTIVE
    USING (request->>'scheme' = 'https');
```

**VERDICT: KEEP PERMISSIVE/RESTRICTIVE stacking completely.** It's the single best idea in RLS.
The OR-of-PERMISSIVE / AND-of-RESTRICTIVE model maps perfectly to HTTP:
"any of these login methods is fine" (OR) vs "you must also be on VPN" (AND).

---

## 2. USING vs WITH CHECK — Read vs Write Filter

### Exact Grammar

```sql
CREATE POLICY name ON table_name
    [ USING ( filter_for_existing_rows ) ]
    [ WITH CHECK ( filter_for_new_row_content ) ]
```

USING filters which rows are **visible** (SELECT, the WHERE-side of UPDATE/DELETE).
WITH CHECK validates the **proposed new state** of a row (INSERT, UPDATE target).
For ALL/UPDATE policies without WITH CHECK, the USING expression doubles as both.

### Real Example

```sql
-- Users see only their own rows; can only insert/update rows they own
CREATE POLICY own_rows ON documents
    USING (owner_id = current_setting('app.user_id')::int)
    WITH CHECK (owner_id = current_setting('app.user_id')::int);

-- Separate read vs write intent (read-only policy has no WITH CHECK)
CREATE POLICY read_public ON articles FOR SELECT
    USING (published = true);
```

### Concept

Splitting the authorization predicate into "what can I see?" vs "what can I write?" lets you express
asymmetric rules: a user might read a resource they can't overwrite, or write to a slot they can't
later view (e.g., one-way audit log appends).

### Why It's a Good Idea

Models a common real-world case (scoped read / narrower write) without requiring two separate policies
with different verbs. The "silent filter" for reads vs "loud error" for writes is also right:
`SELECT` silently omits invisible rows; `INSERT` of an unauthorized shape errors out.

### Mapping to quackapi

HTTP has a natural analog:

| Postgres       | quackapi                                      |
|----------------|-----------------------------------------------|
| `FOR SELECT`   | `FOR GET, HEAD`                               |
| `FOR INSERT`   | `FOR POST`                                    |
| `FOR UPDATE`   | `FOR PUT, PATCH`                              |
| `FOR DELETE`   | `FOR DELETE`                                  |
| `USING`        | predicate over **request context** (auth)     |
| `WITH CHECK`   | predicate over **request body** (input guard) |

```sql
-- quackapi analog: separate read and write auth
CREATE POLICY allow_own_reads ON 'GET /orders/:id' FOR GET
    USING (claims->>'sub' = (SELECT owner_id FROM orders WHERE id = route_params->>'id'));

CREATE POLICY allow_own_writes ON 'PUT /orders/:id' FOR PUT
    USING (claims->>'sub' = route_params->>'sub')
    WITH CHECK (body->>'status' NOT IN ('SHIPPED', 'DELIVERED')); -- can't self-escalate status
```

**VERDICT: KEEP FOR (command filter) and the USING/WITH CHECK distinction.** Map SQL verbs →
HTTP methods. WITH CHECK on HTTP body fields is a novel capability with no mainstream equivalent —
it lets you express "you can write to this route, but only if the body doesn't contain X."

---

## 3. `current_setting()` / `set_config()` — Session Variables as the Injection Mechanism

### Exact Grammar

```sql
-- Read a session variable in a policy predicate
current_setting(setting_name text [, missing_ok boolean]) → text

-- Write a session variable (typically done by the framework before query execution)
set_config(setting_name text, new_value text, is_local boolean) → text
-- is_local = true  → transaction-scoped (cleared on COMMIT/ROLLBACK)
-- is_local = false → session-scoped (persists until session ends)
```

### Real Example (PostgREST production pattern)

```sql
-- Framework sets this BEFORE the user query, inside the same transaction:
SELECT set_config('request.jwt.claims', '{"sub":"u123","role":"member"}', true);
SELECT set_config('request.method', 'GET', true);
SELECT set_config('request.path', '/documents/42', true);

-- RLS policy that reads the injected context:
CREATE POLICY member_sees_own ON documents
    USING (
        owner_id = (current_setting('request.jwt.claims', true)::jsonb->>'sub')::uuid
    );
```

### Concept

The framework injects HTTP request data (JWT claims, headers, path, method) as transaction-scoped
session variables **before** executing the user's query. Policy predicates read these variables with
`current_setting()`. This decouples the injection point (framework) from the consumption point (policy).

### Why It's a Good Idea

- **Zero leakage**: transaction-scope (`is_local = true`) means the injected context evaporates on
  COMMIT/ROLLBACK — a concurrent request can never see another's context
- **Pure SQL policies**: the policy predicate is plain SQL with no special framework APIs; it composes
  with indexes, explain plans, and debuggers naturally
- **Testable in isolation**: you can test a policy by manually calling `set_config()` in a `BEGIN` block
  and running a `SELECT`

### Mapping to quackapi

This is the **direct mechanism** for quackapi. DuckDB doesn't have `set_config()` natively, but
quackapi can implement the same idea:

```sql
-- quackapi runtime injects before the policy is evaluated:
-- SET quack.claims = '{"sub":"u123","scope":"read:orders"}';
-- SET quack.method = 'GET';
-- SET quack.path   = '/orders/42';
-- SET quack.body   = '{"status":"PENDING"}';

CREATE POLICY member_reads_own ON 'GET /orders/:id'
    USING (
        json_extract_string(quack.claims, '$.sub') = route_params->>'id'
    );
```

In DuckDB the equivalent is `getvariable()` / `SET VARIABLE`, though those are not transaction-scoped
today — quackapi's HTTP handler must scope them to the request lifetime some other way (e.g., a
per-request execution context struct, or a synthetic "request schema" table).

**VERDICT: KEEP as the core injection mechanism.** The variable-namespace pattern (`request.*`) is
clean; name it `quack.*` or just make `claims`, `headers`, `method`, `path`, `body` first-class
built-ins visible inside USING expressions.

---

## 4. `TO role` — Role-Scoped Policy Application

### Exact Grammar

```sql
CREATE POLICY name ON table_name
    TO { role_name | PUBLIC | CURRENT_ROLE | CURRENT_USER | SESSION_USER } [, ...]
    USING (...);

-- Roles themselves
CREATE ROLE authenticated NOLOGIN;  -- group role
CREATE ROLE anon NOLOGIN;           -- unauthenticated group role
GRANT authenticated TO joe;         -- joe inherits authenticated's policies
```

### Real Example (Supabase pattern)

```sql
-- anon can only see published articles
CREATE POLICY anon_read ON articles TO anon
    USING (published = true);

-- authenticated users see their own + published
CREATE POLICY auth_read ON articles TO authenticated
    USING (published = true OR author_id = (select auth.uid()));
```

### Concept

A policy only fires for the specified database role(s). Role inheritance means a user inheriting
multiple roles accumulates all matching policies (ORed for PERMISSIVE). Roles replace per-user policy
duplication.

### Why It's a Good Idea

Roles let you say "this whole class of callers gets this policy" without listing every user.
They're the authorization equivalent of type tags on a JWT claim.

### Mapping to quackapi

In HTTP, "roles" naturally map to JWT scopes/claims, not database roles. The `TO` clause maps to
a claim predicate rather than a named role. Two options:

**Option A — Claim-based shorthand (recommended):**
```sql
CREATE POLICY admin_only ON 'DELETE /users/:id'
    TO 'admin'  -- syntactic sugar for: USING (claims->>'role' = 'admin')
    USING (...);
```

**Option B — Named role (quackapi creates a named token-profile):**
```sql
CREATE ROLE anon;     -- unauthenticated (no valid JWT)
CREATE ROLE member;   -- valid JWT, role claim = 'member'
CREATE ROLE admin;    -- valid JWT, role claim = 'admin'

CREATE POLICY admin_writes ON 'DELETE /users/:id' TO admin USING (true);
```

**VERDICT: KEEP `TO` clause but ADAPT it** — in quackapi, TO targets a quackapi-defined "claim profile"
(a named set of JWT claim predicates), not a Postgres database role. Keep the syntax; change the
semantics to match HTTP identity.

---

## 5. PERMISSIVE Default-Deny + No-Policy = Deny

### Postgres Behavior

When RLS is enabled on a table and **no applicable policy matches**, the default is **deny** (no rows
returned, no writes permitted). This is the right default for security.

```sql
ALTER TABLE secret_table ENABLE ROW LEVEL SECURITY;
-- Now: SELECT * FROM secret_table → 0 rows (not an error, just empty)
-- Without any policies: complete lockout
```

### Concept

Enabling RLS flips the default from "allow everything" to "deny everything unless explicitly permitted."
You must actively grant access; forgetting to write a policy doesn't accidentally expose data.

### Why It's a Good Idea

This is the **fail-closed** posture. In web frameworks, the opposite is common: forget to add auth
middleware and the route is wide open. Fail-closed by default catches omissions as 401/403 rather than
silent data exposure.

### Mapping to quackapi

```sql
-- quackapi equivalent: any route without a matching policy is automatically 401
-- Routes are "RLS-enabled by default" — no explicit opt-in needed
-- An explicit "public" route requires an explicit policy:
CREATE POLICY public_health ON 'GET /health' USING (true);
```

**VERDICT: KEEP as the foundational default.** A missing policy = 401. This is the biggest
safety property and the hardest to retrofit if you start open.

---

## 6. `BYPASSRLS` / Table Owner Bypass — Superuser Escape Hatch

### Postgres Grammar

```sql
CREATE ROLE superservice BYPASSRLS;
-- or
ALTER ROLE superservice BYPASSRLS;
```

All superusers and table owners bypass RLS unless `FORCE ROW LEVEL SECURITY` is also set.

### Concept

A trusted system role that completely bypasses the policy layer. Useful for background jobs,
migrations, admin scripts, and the framework's own internal requests.

### Mapping to quackapi

```sql
CREATE ROLE system_job BYPASSRLS;
-- quackapi equivalent:
CREATE ROLE internal;  -- framework-issued tokens (signed with server key) bypass all policies
-- OR: any request with a system-level API key gets quack.role = 'system'
CREATE POLICY allow_internal ON 'GET /admin/:id' TO internal USING (true);
```

**VERDICT: KEEP the concept** — quackapi needs an escape hatch for Temporal workers, cron jobs,
migration scripts. Implement as a privileged token claim (e.g., `claims->>'_internal' = 'true'`)
or a dedicated signing key whose tokens auto-inject `role = 'system'`.

---

## 7. `SECURITY DEFINER` Functions — Encapsulating Policy Logic

### Postgres Pattern

```sql
CREATE FUNCTION auth.uid() RETURNS uuid
    LANGUAGE sql STABLE SECURITY DEFINER
    SET search_path = ''
AS $$
    SELECT (current_setting('request.jwt.claims', true)::jsonb->>'sub')::uuid
$$;

CREATE POLICY own_rows ON documents
    USING (owner_id = auth.uid());  -- clean, reusable
```

### Concept

Wrap recurring claim-extraction logic in a named function so policies stay readable. STABLE marker
tells the planner the function returns the same value for the same inputs within a query —
critical for policy predicates that must not be re-evaluated mid-scan with different results.

### Mapping to quackapi

```sql
-- quackapi built-in helper functions injected into policy scope:
-- caller_id()  → json_extract_string(getvariable('claims'), '$.sub')
-- caller_role() → json_extract_string(getvariable('claims'), '$.role')
-- has_scope(s) → list_contains(json_extract(getvariable('claims'), '$.scopes'), s)

CREATE POLICY own_orders ON 'GET /orders/:id'
    USING (caller_id() = route_params->>'owner_id');

CREATE POLICY require_scope ON 'POST /orders'
    USING (has_scope('orders:write'));
```

**VERDICT: KEEP the helper-function pattern.** Ship quackapi with a small stdlib of claim-extraction
helpers so policy predicates read like English. Don't force users to write raw JSON path expressions.

---

## 8. `ALTER TABLE ... ENABLE/FORCE ROW LEVEL SECURITY`

### Postgres Grammar

```sql
ALTER TABLE t ENABLE ROW LEVEL SECURITY;   -- owners still bypass
ALTER TABLE t FORCE ROW LEVEL SECURITY;    -- owners also gated
ALTER TABLE t DISABLE ROW LEVEL SECURITY;  -- off entirely
```

### Concept

RLS is a per-object opt-in, not global. FORCE closes the owner-bypass loophole.

### Mapping to quackapi

In quackapi, routes don't have an "owner" concept, so the ENABLE/FORCE distinction doesn't apply.
But the opt-in vs opt-out choice matters:

- **Opt-in (ENABLE)**: routes are unsecured until you `CREATE ROUTE ... WITH POLICY`. Better for
  porting an existing API.
- **Opt-out (FORCE / default-deny)**: routes are 401 until you explicitly attach a PERMISSIVE policy.
  Better for new APIs.

**VERDICT: SKIP the ENABLE/FORCE distinction.** quackapi should be default-deny with no per-route
toggle. The FORCE semantics (don't trust the owner either) don't translate.

---

## 9. Policy Names as First-Class Objects

### Postgres Behavior

```sql
DROP POLICY name ON table_name;
ALTER POLICY name ON table_name RENAME TO new_name;
ALTER POLICY name ON table_name USING (...);
-- Visible in pg_policies catalog view
```

Policies are named, independently alterable, and queryable from SQL.

### Mapping to quackapi

This transfers perfectly since quackapi's DDL is stored as live DuckDB tables:

```sql
SELECT * FROM quack_policies;
-- name, route, type, using_expr, check_expr, enabled

ALTER POLICY allow_own_orders USING (caller_id() = route_params->>'id');
DROP POLICY require_scope ON 'POST /orders';
```

**VERDICT: KEEP named policies as catalog rows.** This is exactly what "authorization expressed as
live SQL DDL" means. `quack_policies` becomes a first-class queryable table.

---

## 10. Role Inheritance Chain (INHERIT / NOINHERIT)

### Postgres Grammar

```sql
CREATE ROLE staff NOLOGIN;
CREATE ROLE manager IN ROLE staff;  -- manager inherits staff policies
GRANT staff TO joe;  -- joe also gets staff's policies
```

### Concept

A user who is a member of multiple roles accumulates all of their matching PERMISSIVE policies
(ORed), constrained by all RESTRICTIVE policies (ANDed). Role hierarchy lets you model
"manager can do everything staff can, plus more."

### Mapping to quackapi

HTTP JWTs typically carry flat claims (an array of scopes, a role string), not a role hierarchy.
True role inheritance would require quackapi to maintain its own role-membership graph.

**VERDICT: SKIP for v1.** Model hierarchy via named `CREATE ROLE` with explicit scope lists.
True INHERIT semantics are complex and most HTTP APIs don't need them. Revisit if quackapi
targets enterprise RBAC use cases.

---

## Summary: KEEP vs SKIP

| Concept | Verdict | Reason |
|---------|---------|--------|
| PERMISSIVE / RESTRICTIVE stacking (OR/AND) | **KEEP** | Most powerful RLS idea; maps directly to HTTP auth patterns |
| USING (read filter) / WITH CHECK (write filter) | **KEEP** | Models read-vs-write auth asymmetry; WITH CHECK on body is novel |
| FOR (per-command targeting) | **KEEP** | Map SELECT→GET, INSERT→POST, UPDATE→PUT/PATCH, DELETE→DELETE |
| `current_setting()` / session var injection | **KEEP** | THE mechanism; adapt to DuckDB getvariable() or quack.* namespace |
| Default-deny (no policy = 401) | **KEEP** | Most important safety property |
| Named policies as catalog rows | **KEEP** | Core to "authorization as SQL DDL" |
| Helper functions (auth.uid() pattern) | **KEEP** | Keep policies readable; ship stdlib |
| `TO role` clause | **KEEP (adapted)** | Map to claim profiles, not Postgres roles |
| BYPASSRLS escape hatch | **KEEP** | Framework needs a system-level bypass |
| ENABLE / FORCE RLS distinction | **SKIP** | Default-deny eliminates the need |
| Role INHERIT chain | **SKIP (v1)** | Overkill for JWT-native APIs |
| SECURITY DEFINER on functions | **SKIP** | No privilege escalation concept in HTTP |
| SET ROLE / session impersonation | **SKIP** | Not meaningful in stateless HTTP |

---

## Ranked Top-5 Ideas to Steal

### #1 — PERMISSIVE / RESTRICTIVE Stacking

The OR/AND duality. Multiple PERMISSIVE policies let you write "accept this token type OR that SSO
provider" without a complex OR expression in one predicate. A single RESTRICTIVE policy overlaid on
top says "but always require HTTPS" or "always require MFA flag in claims." This is expressive,
composable, and auditable.

### #2 — Default-Deny (No Policy = 401)

The single most important security posture decision. Routes without a matching PERMISSIVE policy
are automatically 401. Every other framework makes you remember to add auth middleware; quackapi
makes you remember to explicitly open a route. Fail-closed catches mistakes at definition time.

### #3 — session-variable injection as the claims mechanism

`set_config('request.jwt.claims', ..., true)` injected once per transaction, read by policy
predicates via `current_setting()`. In quackapi: the HTTP handler sets `quack.*` variables before
evaluating any policy predicate; predicates read them with built-in helpers or raw `getvariable()`.
This is the cleanest possible decoupling: the framework provides data, the user writes SQL.

### #4 — Named Policies as Catalog Objects (ALTER/DROP/SELECT)

Policies are rows in `quack_policies`, not config files or middleware stacks. You can introspect,
diff, hot-reload, and audit them with plain SQL. This is the flagship feature of "authorization
as live DDL."

### #5 — Helper Function Stdlib (auth.uid() / has_scope())

Wrap claim extraction in named, STABLE functions so policies read like English. `caller_id()`,
`caller_role()`, `has_scope('orders:write')` are far more readable than
`(getvariable('claims')::JSON->>'sub')` in every policy. This makes the system approachable.

---

## Proposed quackapi `CREATE POLICY` Grammar

Synthesizing Postgres RLS + HTTP reality:

```sql
CREATE POLICY policy_name ON 'METHOD /path/pattern'
    [ AS { PERMISSIVE | RESTRICTIVE } ]
    [ FOR { ALL | GET | POST | PUT | PATCH | DELETE | HEAD | OPTIONS } [, ...] ]
    [ TO { role_name | PUBLIC } [, ...] ]
    [ USING ( claim_predicate ) ]
    [ WITH CHECK ( body_predicate ) ]

-- where inside claim_predicate and body_predicate, these built-ins are in scope:
--   claims       → JSON: the verified JWT payload (or NULL if unauthenticated)
--   headers      → JSON: request headers (lowercased)
--   route_params → JSON: path parameters extracted by the route pattern
--   query_params → JSON: URL query string parameters
--   body         → JSON: parsed request body (for POST/PUT/PATCH)
--   caller_id()  → text: shorthand for claims->>'sub'
--   caller_role() → text: shorthand for claims->>'role'
--   has_scope(s) → boolean: true if claims->'scopes' contains s
```

### Full Example Set

```sql
-- 1. Public health check — explicitly opened
CREATE POLICY public_health ON 'GET /health'
    USING (true);

-- 2. Any authenticated user can read their own orders
CREATE POLICY own_orders_read ON 'GET /orders/:id' FOR GET
    USING (caller_id() IS NOT NULL
       AND caller_id() = route_params->>'owner_id');

-- 3. Admins can read any order
CREATE POLICY admin_orders_read ON 'GET /orders/:id' FOR GET TO admin
    USING (caller_role() = 'admin');

-- 4. Write requires a specific scope; body can't escalate status
CREATE POLICY member_writes ON 'PUT /orders/:id' FOR PUT
    USING (has_scope('orders:write'))
    WITH CHECK (body->>'status' NOT IN ('SHIPPED', 'CANCELLED'));

-- 5. Hard gate: all order endpoints require HTTPS
CREATE POLICY require_tls ON 'ALL /orders/:id' AS RESTRICTIVE
    USING (headers->>'x-forwarded-proto' = 'https');

-- 6. Introspect what's active
SELECT policy_name, route, type, for_methods, using_expr, check_expr
FROM quack_policies
WHERE route LIKE '/orders%'
ORDER BY type, policy_name;

-- 7. Hot-reload a policy without restarting
ALTER POLICY own_orders_read ON 'GET /orders/:id'
    USING (caller_id() IS NOT NULL
       AND caller_id() = route_params->>'owner_id'
       AND claims->>'email_verified' = 'true');

-- 8. System bypass (Temporal worker, cron)
CREATE ROLE internal;
CREATE POLICY internal_bypass ON 'ALL /orders/:id' TO internal
    USING (true);
```

### Stacking Semantics (quackapi)

```
Final authorization = true iff:
  (AT LEAST ONE matching PERMISSIVE USING is satisfied)
  AND (ALL matching RESTRICTIVE USING expressions are satisfied)
  AND (if body present: WITH CHECK of matching policy is satisfied)
```

A route with no matching PERMISSIVE policy → 401 (not 403 — we don't even confirm the route exists
to an unauthenticated caller). A route where PERMISSIVE passes but RESTRICTIVE fails → 403.

### On `TO role` semantics

In quackapi, `TO role_name` means "this policy only applies if `caller_role() = 'role_name'`" — it
is syntactic sugar over the claim predicate. Under the hood quackapi generates:

```sql
-- CREATE POLICY x ON '...' TO admin USING (expr)
-- is equivalent to:
-- CREATE POLICY x ON '...' USING (caller_role() = 'admin' AND (expr))
```

`TO PUBLIC` (default) means no role constraint — the USING predicate alone governs.

---

## Key Differences from Postgres RLS

| Dimension | Postgres RLS | quackapi CREATE POLICY |
|-----------|--------------|------------------------|
| Target object | Database table | HTTP route pattern |
| Identity source | `current_user` (database role) | JWT claims in `quack.claims` |
| Context injection | `set_config()` in same txn | Framework sets `quack.*` vars per request |
| "Roles" | Postgres roles (GRANT/INHERIT) | Claim profiles (syntactic sugar) |
| Silent filter | SELECT hides rows | GET returns 404 for non-owned resources |
| Write error | `ERROR: policy violation` | 422 Unprocessable / 403 Forbidden |
| No policy = | default deny (no rows) | 401 Unauthorized |
| Catalog | `pg_policies` | `quack_policies` table |
| Hot reload | `ALTER POLICY` + reconnect | `ALTER POLICY` takes effect immediately |
