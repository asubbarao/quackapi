# ClickHouse DDL for Access Control, Governance, and Quotas — Research for quackapi

**Platform researched:** ClickHouse (official docs as of 2026).
**Scope:** CREATE USER, CREATE ROLE, CREATE ROW POLICY, CREATE QUOTA, CREATE SETTINGS PROFILE, GRANT, CREATE NAMED COLLECTION, and related auth mechanisms (IDENTIFIED variants + JWT).
**Goal:** Extract good ideas from mature data-platform DDL for a SQL-native web framework (quackapi) that expresses server concerns as live, mutable SQL DDL inside DuckDB. Focus on introspection (everything visible in tables), runtime mutability, and enforcement by the SQL engine.
**Sources:** Official ClickHouse documentation (clickhouse.com/docs/sql-reference/statements/create/*, operations/access-rights, operations/quotas, operations/named-collections, operations/external-authenticators/jwt, system tables).

All entities created via DDL are stored in the access control storage and are **fully introspectable** via `system.*` tables (e.g., `system.users`, `system.roles`, `system.row_policies`, `system.quotas`, `system.settings_profiles`, `system.grants`, `system.quotas_usage`). SHOW CREATE equivalents exist. Changes are live and cluster-propagated via ON CLUSTER.

---

## CREATE USER

### Exact DDL Grammar + Real Example

```sql
CREATE USER [IF NOT EXISTS | OR REPLACE] name1 [, name2 [,...]] [ON CLUSTER cluster_name]
    [NOT IDENTIFIED | IDENTIFIED {[WITH {plaintext_password | sha256_password | sha256_hash | double_sha1_password | double_sha1_hash | bcrypt_password | ...}] BY {'password' | 'hash'}} 
     | WITH NO_PASSWORD 
     | {WITH ldap SERVER 'server_name'} 
     | {WITH kerberos [REALM 'realm']} 
     | {WITH ssl_certificate CN 'common_name' | SAN 'TYPE:subject_alt_name'} 
     | {WITH ssh_key BY KEY 'public_key' TYPE 'ssh-rsa|...'} 
     | {WITH http SERVER 'server_name' [SCHEME 'Basic']} 
     | ... ] [VALID UNTIL datetime]
    [HOST {LOCAL | NAME 'name' | REGEXP 'name_regexp' | IP 'address' | LIKE 'pattern'} [,...] | ANY | NONE]
    [VALID UNTIL datetime]
    [IN access_storage_type]
    [ROLE role [,...]]
    [DEFAULT ROLE role [,...] | ALL [EXCEPT ...]]
    [DEFAULT DATABASE database | NONE]
    [GRANTEES {user | role | ANY | NONE} [,...] [EXCEPT {user | role} [,...]]]
    [SETTINGS variable [= value] [MIN [=] min_value] [MAX [=] max_value] [READONLY | WRITABLE] | PROFILE 'profile_name'] [,...]
```

**Real examples (from docs):**
```sql
-- Basic password user, restricted host, with roles + settings profile
CREATE USER mira 
  HOST IP '127.0.0.1' 
  IDENTIFIED WITH sha256_password BY 'qwerty'
  ROLE analyst, reader 
  DEFAULT ROLE analyst
  DEFAULT DATABASE analytics
  SETTINGS max_memory_usage = 10000000000 MIN 1000000000 MAX 20000000000;

-- Multiple auth methods + expiration
CREATE USER api_svc 
  IDENTIFIED WITH sha256_password BY 'p1', bcrypt_password BY 'p2' 
  VALID UNTIL '2025-12-31 23:59:59';

-- No password (internal) + host restriction
CREATE USER internal_app NOT IDENTIFIED HOST LOCAL;
```

**System table exposure:** `system.users` (auth_type array, auth_params JSON, valid_until, host_*, default_roles_*, grantees_*, default_database).

### Concept in One Line
Declarative user account with pluggable authentication, host restrictions, role inheritance, default database, grantee delegation limits, and per-user settings with constraints — all as a single DDL statement.

### WHY It's a Good Idea
- **Everything is DDL and live-mutable** (ALTER USER exists symmetrically). No separate config files for core auth after enabling access_management.
- **Multiple authentication methods** on one principal (password variants + external + certs) with VALID UNTIL per-method or global.
- **Host binding** as first-class (IP, name, regexp, LIKE) — prevents credential replay from wrong networks.
- **GRANTEES** clause is a powerful delegation control (prevent privilege escalation chains).
- **SETTINGS with MIN/MAX/READONLY** on the user itself provide immediate guardrails.
- **Introspectable** — `SELECT * FROM system.users` gives the full picture without parsing config XML.
- **ON CLUSTER** for distributed DDL consistency.

### Mapping to quackapi (Server Primitive It Informs)
- **CREATE AUTH** (or CREATE PRINCIPAL / IDENTITY): The IDENTIFIED clause family + VALID UNTIL directly informs JWT / token / secret verification. DuckDB crypto + CREATE SECRET could supply the verification material; host restrictions map to request origin / IP / header claims.
- **CREATE POLICY** (base): Roles + DEFAULT ROLE + direct SETTINGS become a composable "identity bundle" attached to routes or global.
- **Request context**: A user maps to a set of claims that later CREATE POLICY / CREATE QUOTA / CREATE SETTINGS PROFILE can reference (e.g., current_user(), getSetting()).
- **Introspection endpoint / table**: Every principal appears as a row in a live `system.quack_users` (or equivalent) table that route handlers or policies can JOIN against.

**RED-TEAM verdict: KEEP**  
Strongly worth stealing for quackapi. The multi-method auth + expiration + host + GRANTEES + live system table are excellent primitives. SKIP only the enterprise external (ldap/kerberos) boilerplate unless you add pluggable authenticators later.

---

## CREATE ROLE

### Exact DDL Grammar + Real Example

```sql
CREATE ROLE [IF NOT EXISTS | OR REPLACE] name1 [, name2 [,...]] [ON CLUSTER cluster_name]
    [IN access_storage_type]
    [SETTINGS variable [= value] [MIN [=] min_value] [MAX [=] max_value] [CONST|READONLY|WRITABLE|CHANGEABLE_IN_READONLY] | PROFILE 'profile_name'] [,...]
```

**Real example:**
```sql
CREATE ROLE accountant;
GRANT SELECT ON db.* TO accountant;

-- Later
GRANT accountant TO mira;
SET ROLE accountant;
SELECT * FROM db.*;   -- works
```

Roles are containers for privileges + settings. A user can have multiple roles active (SET ROLE); final privilege set is union. Default roles applied at login.

**Introspection:** `system.roles`, `system.grants` (shows grants to roles).

### Concept in One Line
A named, grantable bundle of privileges and constrained settings that can be assigned to users or other roles.

### WHY It's a Good Idea
- **Composition over duplication**: Define once, assign many times. Privileges granted to role automatically apply to assignees.
- **SETTINGS on roles** + constraints (CONST/READONLY/CHANGEABLE_IN_READONLY) let you ship "safe profiles" with the role.
- **Hierarchy via role-to-role grants** (GRANT role TO another_role).
- **Dynamic activation**: `SET ROLE` (or multiple) lets a principal choose a subset at runtime — powerful for apps with different "modes".
- **Live and queryable**: Roles and their grants are rows you can SELECT/JOIN.

### Mapping to quackapi
- **CREATE POLICY** or **CREATE ROLE** (as first-class server object): Roles become reusable policy/claim sets or "auth bundles".
- In a web framework: `GRANT SELECT ON /api/orders TO analyst_role;` could be expressed as route-level policy attachment.
- **CREATE AUTH** or identity attachment: Assign roles at JWT claim processing time or via `CREATE USER ... ROLE ...`.
- **Runtime**: A request's effective role set (default + SET ROLE equivalent via header/claims) determines which policies and rate limits apply.
- **Introspectable governance**: `SELECT * FROM system.roles JOIN system.grants ...` becomes the source of truth for "who can do what".

**RED-TEAM verdict: KEEP**  
Excellent. Roles + role-to-role + settings-with-constraints on roles are high-signal ideas. In quackapi this directly feeds CREATE POLICY composition and perhaps "named policy sets".

---

## CREATE ROW POLICY

### Exact DDL Grammar + Real Example

```sql
CREATE [ROW] POLICY [IF NOT EXISTS | OR REPLACE] policy_name1 [ON CLUSTER cluster_name1] ON [db1.]table1|db1.*
        [, policy_name2 ...]
    [IN access_storage_type]
    [FOR SELECT] USING condition
    [AS {PERMISSIVE | RESTRICTIVE}]
    [TO {role1 [, role2 ...] | ALL | ALL EXCEPT role1 [, role2 ...]}]
```

**Real examples:**
```sql
-- Tenant isolation
CREATE ROW POLICY tenant_101_policy 
  ON multitenant.orders 
  FOR SELECT 
  USING tenant_id = 101 
  TO tenant_101;

-- Permissive + restrictive combination (AND for restrictive)
CREATE ROW POLICY pol1 ON mydb.table1 USING b=1 TO mira, peter;
CREATE ROW POLICY pol2 ON mydb.table1 USING c=2 AS RESTRICTIVE TO peter, antonio;
-- peter sees only rows where b=1 AND c=2

-- Database-wide + table override
CREATE ROW POLICY db_filter ON mydb.* USING tenant_id = currentTenant() TO app_role;
```

**Key mechanics:**
- USING is an arbitrary SQL boolean expression (can call functions, settings, etc.).
- Multiple policies combine with OR for permissive, AND for restrictive.
- Database policies + table policies are combined.
- "FOR SELECT" (default for read filtering); note docs warn that row policies are only meaningful for readonly users.

**Introspection:** `system.row_policies` (select_filter, is_restrictive, apply_to_list, etc.).

### Concept in One Line
A named, attachable SQL predicate (USING condition) that is automatically AND/OR-ed into queries for matching users/roles on specific tables (or DBs).

### WHY It's a Good Idea
- **True row-level security expressed in SQL** — no app-layer filtering that can be bypassed.
- **Composable via PERMISSIVE/RESTRICTIVE** semantics (standard in mature RLS systems).
- **Dynamic expressions**: `USING tenant_id = getSetting('tenant_id')` or function calls lets policies be driven by session state without hard-coding values at CREATE time.
- **Scoped application** (per table or DB wildcard) + TO clause gives fine targeting.
- **Introspectable and live**: Policies are data you can query, audit, and alter at runtime.
- **Engine-enforced**: The predicate is pushed into the query plan by ClickHouse.

### Mapping to quackapi (Server Primitive It Informs)
- **CREATE POLICY** (core differentiator): This is almost 1:1. In quackapi, `CREATE POLICY tenant_isolation ON /api/orders USING claims.tenant_id = orders.tenant_id TO authenticated;` or similar.
- **Authorization as SQL predicate over request claims**: Exactly. The "claims" object (from JWT or auth) becomes available as a struct/row in the USING expression (DuckDB SQL).
- **Enforcement point**: Before or during route handler execution (or as a view rewrite / filter on underlying "tables" if modeling resources as relations).
- **Composition model**: Adopt PERMISSIVE/RESTRICTIVE or a clean equivalent.
- **Dynamic via settings/claims**: `getSetting('X')` or equivalent DuckDB macro for request context.

**RED-TEAM verdict: KEEP (high priority)**  
This is one of the best ideas in the set. Row-level security expressed as live SQL DDL that the engine enforces is extremely powerful and directly maps to "CREATE POLICY" as a first-class web-framework primitive. The only potential SKIP is the "readonly only" limitation warning — in a web framework you may need to think about write paths too (e.g., ownership checks).

---

## CREATE QUOTA (Directly Maps to CREATE RATE LIMIT)

### Exact DDL Grammar + Real Example

```sql
CREATE QUOTA [IF NOT EXISTS | OR REPLACE] name [ON CLUSTER cluster_name]
    [IN access_storage_type]
    [KEYED BY {user_name | ip_address | forwarded_ip_address | client_key | client_key,user_name | client_key,ip_address | normalized_query_hash} | NOT KEYED]
    [IPV4_PREFIX_BITS number]
    [IPV6_PREFIX_BITS number]
    [FOR [RANDOMIZED] INTERVAL number {second | minute | hour | day | week | month | quarter | year}
        {MAX { {queries | query_selects | query_inserts | errors | result_rows | result_bytes | read_rows | read_bytes | written_bytes | execution_time | failed_sequential_authentications | queries_per_normalized_hash} = number } [,...] |
         NO LIMITS | TRACKING ONLY} [,...]]
    [TO {role [,...] | ALL | ALL EXCEPT role [,...]}]
```

**Real examples (from docs):**
```sql
-- Simple per-user quota
CREATE QUOTA qA FOR INTERVAL 15 month MAX queries = 123 TO CURRENT_USER;

-- Multiple intervals + multiple limits
CREATE QUOTA qB 
  FOR INTERVAL 30 minute MAX execution_time = 0.5, 
  FOR INTERVAL 5 quarter MAX queries = 321, errors = 10 
  TO default;

-- Keyed by normalized query hash (per-pattern rate limit)
CREATE QUOTA qC KEYED BY normalized_query_hash FOR INTERVAL 1 hour MAX queries = 100 TO default;

-- Per-pattern hard cap regardless of key
CREATE QUOTA qD FOR INTERVAL 1 hour MAX queries_per_normalized_hash = 50 TO default;
```

**Usage & enforcement:**
- Quotas track across distributed queries.
- When any limit in any active interval is hit, further queries are rejected with a message indicating which quota/interval.
- `KEYED BY` creates separate buckets (per user, per IP, per query pattern, etc.).
- `normalized_query_hash` is particularly clever: literals are normalized so `SELECT 1` and `SELECT 2` share a bucket.
- Live usage visible in `system.quotas_usage` (current counts, max_*, start/end times, is_current).
- `system.quotas` shows the definition.

Also supports RANDOMIZED intervals and TRACKING ONLY (no enforcement).

### Concept in One Line
A named, multi-window resource governor that tracks and/or caps consumption metrics (queries, bytes, time, errors, per-query-pattern, auth failures, etc.) with flexible bucketing (by user/IP/query-hash) and TO assignment to users/roles.

### WHY It's a Good Idea
- **Multi-resolution windows**: Different limits for "last 5 minutes" vs "last day" vs "last quarter" in one object.
- **Rich metrics**: Not just "requests/sec" — includes read/write volume, execution time, errors, auth failures, and per-normalized-query limits. Excellent for abuse prevention and fair sharing.
- **KEYED BY + prefix bits**: Sophisticated bucketing without app code (per-tenant, per-IP subnet, per-query-shape).
- **Normalized query bucketing**: Prevents "unique query explosion" attacks while still allowing different expensive patterns to have separate budgets.
- **TRACKING ONLY vs enforcement** + RANDOMIZED: Great for gradual rollout and observability.
- **Fully introspectable live usage**: `system.quotas_usage` shows exactly where you are against every window — perfect for dashboards and self-service "why am I throttled?".
- **TO assignment + roles**: Same composable targeting as policies/roles.
- **Distributed-aware**: Accumulates across shards.

### Mapping to quackapi (Server Primitive It Informs)
- **CREATE RATE LIMIT** (or CREATE QUOTA): Almost direct transplant.
  - Windows → multiple `FOR INTERVAL` clauses.
  - Metrics → map to HTTP concepts: `requests`, `response_bytes`, `request_bytes`, `errors`, `handler_time`, `auth_failures`, perhaps `route_specific` counters.
  - KEYED BY → `BY client_ip`, `BY jwt.sub`, `BY jwt.tenant_id`, `BY route_pattern` (or normalized request fingerprint).
  - Per-route or global: `CREATE QUOTA api_heavy FOR INTERVAL 1 minute MAX requests=100, errors=10 TO public_role;`
- **Enforcement in request lifecycle**: Before/after handler, using DuckDB state or lightweight counters in tables.
- **Introspection**: `SELECT * FROM system.quota_usage WHERE is_current` becomes a `/limits` or admin endpoint.
- **Dynamic assignment**: Attach quotas in CREATE USER / CREATE ROLE or via claims (similar to JWT grants in ClickHouse).
- **Abuse / DoS protection + fair use** for a web API without writing custom middleware.

**RED-TEAM verdict: KEEP (very high priority — study this one closely)**  
This is one of the strongest candidates. Quota design is richer than most simple rate-limiters. The combination of multi-interval, rich counters, sophisticated keys (especially normalized_query_hash equivalent for API "query shape"), live usage tables, and DDL mutability is gold for a SQL-native web framework. Minimal bloat; most of it maps cleanly to HTTP concerns. The only enterprise bits (quarter/year intervals, randomized) are cheap to keep or ignore.

---

## CREATE SETTINGS PROFILE

### Exact DDL Grammar + Real Example

```sql
CREATE SETTINGS PROFILE [IF NOT EXISTS | OR REPLACE] name1 [, name2 [,...]] 
    [ON CLUSTER cluster_name]
    [IN access_storage_type]
    [SETTINGS variable [= value] [MIN [=] min_value] [MAX [=] max_value] [CONST|READONLY|WRITABLE|CHANGEABLE_IN_READONLY] | INHERIT 'profile_name'] [,...]
    [TO {{role1 | user1 [, role2 | user2 ...]} | NONE | ALL | ALL EXCEPT {role1 | user1 [, role2 | user2 ...]}}]
```

**Real example:**
```sql
CREATE USER robin IDENTIFIED BY 'password';

CREATE SETTINGS PROFILE max_memory_usage_profile 
  SETTINGS max_memory_usage = 100000001 MIN 90000000 MAX 110000000
  TO robin;

-- Inheritance example
CREATE SETTINGS PROFILE base_profile SETTINGS max_execution_time = 60;
CREATE SETTINGS PROFILE prod_profile INHERIT 'base_profile' SETTINGS max_memory_usage = 16e9 TO prod_role;
```

Constraints:
- MIN/MAX value bounds.
- CONST / READONLY / WRITABLE / CHANGEABLE_IN_READONLY control mutability even in readonly mode for some settings.

**Introspection:** `system.settings_profiles`, `system.settings_profile_elements`.

### Concept in One Line
A named, inheritable, assignable bundle of server/client settings with value constraints and mutability rules.

### WHY It's a Good Idea
- **Guardrails as data**: MIN/MAX + READONLY etc. prevent runaway queries or misconfiguration without code changes.
- **Inheritance** reduces duplication.
- **TO assignment** (same targeting model as quotas/policies) + roles/users.
- **Live and queryable**.
- Separates "resource policy" from "privilege policy".

### Mapping to quackapi
- **CREATE SETTINGS PROFILE** or **part of CREATE POLICY / CREATE AUTH**: Limits on request size, timeout, concurrency, feature flags, etc.
- In web terms: `max_request_body_bytes`, `handler_timeout_seconds`, `max_concurrent_for_user`, `enable_beta_feature`.
- **Request context injection**: Settings become available inside route handlers via DuckDB `getSetting()` or equivalent (very similar to how ClickHouse row policies and quotas read settings).
- **Safety for multi-tenant APIs**: Assign restrictive profiles to untrusted roles.

**RED-TEAM verdict: KEEP**  
Good supporting primitive. In a web framework, the "settings with constraints + inheritance + assignment" pattern is useful for per-principal or per-role configuration. Less critical than QUOTA or ROW POLICY, but low cost to adopt. The CONST/CHANGEABLE_IN_READONLY nuance may be overkill (SKIP or simplify).

---

## GRANT

### Exact DDL Grammar + Real Example

**Privilege grant:**
```sql
GRANT [ON CLUSTER cluster_name] privilege[(column_name [,...])] [,...] 
  ON {db.table[*]|db[*].*|*.*|table[*]|*} 
  TO {user | role | CURRENT_USER} [,...] 
  [WITH GRANT OPTION] [WITH REPLACE OPTION]
```

**Role grant:**
```sql
GRANT [ON CLUSTER cluster_name] role [,...] 
  TO {user | another_role | CURRENT_USER} [,...] 
  [WITH ADMIN OPTION] [WITH REPLACE OPTION]
```

**CURRENT GRANTS** (copy current user's grants):
```sql
GRANT CURRENT GRANTS ... TO ...
```

**Wildcard / prefix grants** (24.10+):
- `GRANT SELECT ON db.my_tables* TO john;`
- `GRANT SELECT ON db*.* TO john;`

Huge privilege hierarchy (ALL > groups > specific like SELECT, INSERT, ALTER*, CREATE*, SYSTEM*, SOURCES*, NAMED COLLECTION*, etc.). Privileges have levels (COLUMN, TABLE, DATABASE, GLOBAL, ...).

Many examples in docs, including column-level, wildcard prefix, and access-management privileges.

**Introspection:** `system.grants` (very detailed: access_type, database, table, column, grant_option, is_wildcard, is_partial_revoke, etc.).

### Concept in One Line
SQL statement that assigns fine-grained, hierarchical, optionally delegable privileges (or roles) on objects (tables, DBs, columns, global actions, sources, etc.) with wildcard support and live effect.

### WHY It's a Good Idea
- **Hierarchical privileges** with clear semantics (granting ALTER gives most but not all sub-ALTERs — explicit sub-privs still needed for some).
- **WITH GRANT OPTION** + GRANTEES on users provides controlled delegation.
- **Prefix wildcards** (`db*.*`, `foo*`) are pragmatic for large schemas.
- **REPLACE vs append** semantics explicit.
- **Everything queryable** in `system.grants`.
- **Privileges can be granted on non-existent objects** (future-proofing).

### Mapping to quackapi
- **Route / resource authorization**: `GRANT GET,POST ON /api/orders* TO analyst_role;` or modeled as privileges on "resources".
- **Fine-grained within handlers**: Column-like (field-level) or action-level grants.
- **CREATE POLICY** often builds on top of granted privileges.
- **Admin surfaces**: An admin can GRANT specific routes or methods without code deploy.
- **WITH GRANT OPTION** for self-service delegation within tenants.

**RED-TEAM verdict: KEEP (core)**  
The GRANT model (especially with wildcards and introspection) is table stakes for any serious access system. In quackapi, adapt the object model from tables/columns to routes/resources + actions. The hierarchy and delegation controls are worth copying. Enterprise volume of SYSTEM/ALTER sub-privs can be simplified, but the mechanism is solid.

---

## CREATE NAMED COLLECTION + Auth Mechanisms

### CREATE NAMED COLLECTION

**Grammar:**
```sql
CREATE NAMED COLLECTION [IF NOT EXISTS] name [ON CLUSTER cluster] AS
  key_name1 = 'some value' [[NOT] OVERRIDABLE],
  key_name2 = 'some value' [[NOT] OVERRIDABLE],
  ...
```

**Example:**
```sql
CREATE NAMED COLLECTION s3_mydata AS
  access_key_id = 'AKIA...',
  secret_access_key = 'wJalr...',
  format = 'CSV',
  url = 'https://...';

-- Usage (no secrets repeated)
INSERT INTO FUNCTION s3(s3_mydata, filename=..., format='TSV'...) ...
CREATE TABLE ... ENGINE = S3(s3_mydata, ...);
```

- OVERRIDABLE / NOT OVERRIDABLE + global `allow_named_collection_override_by_default`.
- Can be stored local / Keeper / encrypted.
- Requires `named_collection_control` (and SHOW variants).
- ALTER / DROP exist.
- Used to hide credentials while allowing parameterized use by less-privileged users.

**Introspection:** `system.named_collections` (and SECRETS variant when allowed).

### Auth Mechanisms (CREATE USER IDENTIFIED family + JWT)

From CREATE USER:
- Passwords: plaintext (discouraged), sha256, bcrypt, double_sha1, hashes with salt.
- External: ldap, kerberos, ssl_certificate, ssh_key, http SERVER.
- Multiple methods per user.
- VALID UNTIL per auth or global.
- HOST restrictions.

**JWT (ClickHouse Cloud, special):**
- No `CREATE USER ... IDENTIFIED WITH jwt`.
- Token (HS256/RS256/ES256) presented via Bearer / native protocol.
- Required claims: iss, sub, aud, exp, iat, alg, kid (for JWKS).
- Optional claims: `clickhouse:grants` (array of grant fragments), `clickhouse:roles`.
- Creates **ephemeral in-memory users** with deterministic UUID from iss+sub+aud.
- Access rights = permission_limit ∩ (token_grants ∪ token_roles).
- Persistent assignments (profiles, quotas, policies) can be attached to the stable UUID/username.
- Token freshness via iat tracking.
- Garbage-collected after exp.
- Built-in Cloud authenticator + per-provider config.

This is a modern "token brings its own (capped) identity + grants" model.

### Concept in One Line
Named collections = secure, reusable, overridable credential/config bags. Auth mechanisms = pluggable identity + (for JWT) dynamic, claim-driven ephemeral principals with embedded grants/roles.

### WHY It's a Good Idea
- **Secret hygiene**: Named collections let you avoid putting keys in every query/table definition; override control prevents leakage.
- **JWT ephemeral model** is elegant for short-lived, claim-driven access without provisioning users.
- `clickhouse:grants` / `clickhouse:roles` claims turn the token into a portable capability.
- Stable UUID allows persistent policy/setting attachment even though the runtime user is transient.
- iat tracking + GC prevent replay and resource leaks.

### Mapping to quackapi
- **CREATE SECRET / NAMED COLLECTION equivalent** (as mentioned in the quackapi goal): `CREATE NAMED COLLECTION s3_creds AS ...` or `CREATE SECRET aws_s3 ...` that routes/handlers reference by name. DuckDB secrets integration is perfect here.
- **CREATE AUTH** for JWT: Parse token (DuckDB crypto or extension), extract claims, synthesize an ephemeral principal or directly produce a claims struct for policies.
- **Embedded grants/roles in token** → directly feed quackapi's CREATE POLICY and GRANT-like mechanism.
- **Host + VALID UNTIL** ideas apply to token validation + session lifetime.
- Named collections for external service config (DBs, object storage, etc.) referenced safely from route definitions or jobs.

**RED-TEAM verdict:**
- **NAMED COLLECTION: KEEP** — excellent pattern for secret management inside SQL. Very relevant.
- **JWT model: KEEP** (with adaptation) — the claim-embedded grants + roles + ephemeral + persistent policy attachment is a modern, clean idea. The full external authenticator surface (ldap etc.) can mostly be **SKIPPED** for a lightweight web framework unless you want pluggable IDPs. Password hashing variants are implementation details; focus on pluggable verification.

---

## Cross-Cutting Strengths Visible Across These DDLs

- **Everything is a table**: `system.users`, `system.grants`, `system.row_policies`, `system.quotas`, `system.quotas_usage`, `system.settings_profiles`, `system.roles`, etc. You can SELECT, JOIN, build admin UIs, and audit purely in SQL.
- **Live + mutable**: CREATE / ALTER / DROP are the source of truth; changes take effect immediately.
- **Consistent targeting model**: Most governance objects use `TO {users/roles | ALL | ALL EXCEPT ...}`.
- **ON CLUSTER** for distributed safety.
- **Inheritance and composition** (roles, profile INHERIT, permissive/restrictive policies).
- **Constraints + safety knobs** built into the DDL (MIN/MAX, READONLY, OVERRIDABLE, GRANTEES, permission_limit for JWT).
- **Rich observability**: quotas_usage, grants, etc. give runtime state, not just config.

---

## RED-TEAM Summary (KEEP vs SKIP)

**Strong KEEP (steal these):**
- CREATE QUOTA (multi-window, rich metrics, sophisticated KEYED BY including normalized query hash, live usage table) → direct CREATE RATE LIMIT.
- CREATE ROW POLICY (SQL predicate USING + PERMISSIVE/RESTRICTIVE composition + dynamic via settings) → core CREATE POLICY.
- GRANT model + wildcard/prefix + WITH GRANT OPTION + system.grants introspection.
- Roles as composable privilege/setting containers (with settings constraints).
- CREATE USER host restrictions + multi-auth + VALID UNTIL + GRANTEES + direct settings.
- Named collections for credential/config abstraction (with overridable control).
- JWT-style claim-driven ephemeral identity with embedded grants/roles + stable UUID for persistent policies.
- Universal system.* table exposure of all access entities and live usage.

**KEEP with simplification:**
- CREATE SETTINGS PROFILE (inheritance + constraints). Simplify the mutability flags.
- Privilege hierarchy (flatten for web routes/actions).

**Mostly SKIP (enterprise bloat for a web framework):**
- Full zoo of external authenticators (ldap, kerberos, ssh_key, ssl cert) — keep pluggable interface only.
- Very long tail of ClickHouse-specific SYSTEM/ALTER sub-privileges.
- Some quota intervals (quarters) or randomized unless you have the use case.
- Inter-server secret mechanics, ZooKeeper-backed storage details.
- Column-level grants in their exact form (adapt concept to response field filtering or resource sub-paths).

---

## Ranked Shortlist — Top 5 Ideas Worth Stealing for quackapi

1. **CREATE QUOTA with multi-interval windows + rich metrics + KEYED BY (incl. normalized query hash) + live system.quotas_usage**  
   Best-in-class rate limiting / resource governance expressed in DDL. Directly becomes CREATE RATE LIMIT. The per-pattern bucketing and usage introspection are especially valuable.

2. **CREATE ROW POLICY as a live SQL USING predicate with PERMISSIVE/RESTRICTIVE composition and TO targeting**  
   The heart of "authorization as a SQL predicate over request claims". Maps almost perfectly to CREATE POLICY. Engine-enforced, dynamic, introspectable, composable.

3. **GRANT (hierarchical + wildcard/prefix + WITH GRANT OPTION + full system.grants table)**  
   Declarative, auditable, delegable permission assignment that works even on non-existent objects. Foundation for route/resource auth.

4. **Roles + CREATE USER (roles, default roles, settings on both, host restrictions, GRANTEES, multi-method IDENTIFIED + VALID UNTIL)**  
   Principals and bundles as first-class mutable SQL objects. Provides the identity + delegation model for CREATE AUTH and policy attachment.

5. **Named Collections + JWT claim-embedded grants/roles + ephemeral users with persistent policy attachment**  
   Two related ideas: (a) safe reusable secret/config bags (CREATE SECRET/NAMED COLLECTION), and (b) modern token-driven identity where the token carries (capped) grants/roles while stable UUIDs allow durable policies/quotas/profiles. Perfect for quackapi's CREATE AUTH + DuckDB CREATE SECRET story.

Honorable mentions: Settings profiles with constraints, universal system.* introspection of governance state, ON CLUSTER / distributed DDL safety patterns, and the consistent "TO ALL / ALL EXCEPT" targeting language.

---

**Conclusion for quackapi design:**  
ClickHouse demonstrates that a data platform can make security, rate limiting, and configuration first-class, live, SQL DDL citizens that are as queryable and composable as tables. Steal the quota design, row policy predicate model, grant/role mechanics, named collection secret abstraction, and the JWT "token carries claims + grants" approach. Keep the implementation lean for a web framework — focus on routes/resources instead of tables/columns, claims instead of users, and HTTP metrics instead of read_rows/execution_time. The result will feel native to anyone who likes doing governance in SQL.

Report generated 2026-07-02. All syntax taken directly from the cited ClickHouse documentation pages.