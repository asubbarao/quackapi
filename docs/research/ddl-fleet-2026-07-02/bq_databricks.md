# quackapi Research: Stealing DDL Ideas from BigQuery and Databricks Unity Catalog

**Goal**: Identify high-quality, declarative DDL patterns for policies, authorization, governance, masking, sharing, and access control from mature data platforms. Evaluate each for applicability to quackapi — a DuckDB-native, SQL-DDL-driven web framework where server concerns (routes, policies, auth, validation, limits) are expressed as live, mutable, introspectable SQL DDL.

**Scope**: Focus on specified features plus closely related governance/authorization DDL. Cover exact grammar + real examples, one-line concept, why good, quackapi mapping (which primitive it informs: e.g. CREATE POLICY, CREATE AUTH, rate limiting, CORS-like external access, etc.). Red-team: KEEP vs SKIP.

**Sources**: Official documentation (BigQuery DDL reference, row-level security, column-level security + data masking; Databricks Unity Catalog row filters/column masks, ABAC CREATE POLICY, CREATE SHARE, GRANT, dynamic views). Syntax captured faithfully from 2026 docs.

All items evaluated for a lightweight, runtime-SQL web framework (no heavy enterprise IAM sprawl, prefer engine-enforced predicates, first-class DDL mutability, table/view introspection, minimal side-band APIs).

---

## BigQuery

### 1. CREATE ROW ACCESS POLICY

**Exact DDL Grammar + Example** (from official DDL ref + managing docs):

```
CREATE [ OR REPLACE ] ROW ACCESS POLICY [ IF NOT EXISTS ] policy_name
ON [project_id.]dataset_id.table_id
GRANT TO ( 'principal_specifier' [, ...] )
FILTER USING ( boolean_expression );
```

Real examples:

```sql
-- Basic region filter for a group
CREATE ROW ACCESS POLICY apac_filter
ON project.dataset.my_table
GRANT TO ('group:sales-apac@example.com')
FILTER USING (region = 'APAC');

-- Personal data + domain + SESSION_USER()
CREATE ROW ACCESS POLICY salary_personal
ON dataset1.table1
GRANT TO ("domain:example.com")
FILTER USING (email = SESSION_USER());

-- Subquery-driven (lookup table) + OR REPLACE
CREATE OR REPLACE ROW ACCESS POLICY apac_filter
ON project.dataset.my_table
GRANT TO ('domain:example.com')
FILTER USING (region IN (
  SELECT region FROM lookup_table WHERE email = SESSION_USER()
));

-- Multiple policies on one table (union semantics for grantee)
CREATE ROW ACCESS POLICY shoes
ON project.dataset.my_table
GRANT TO ('user:abc@example.com')
FILTER USING (product_category = 'shoes');

CREATE ROW ACCESS POLICY blue_products
ON project.dataset.my_table
GRANT TO ('user:abc@example.com')
FILTER USING (color = 'blue');
```

`DROP ROW ACCESS POLICY policy_name ON table;` exists. Up to 100 policies/table. Policies combine with OR for a principal (any matching policy grants the rows).

**Concept (one line)**: Attach one or more named boolean filter predicates to a table; only rows where the predicate is true (for the querying principal) are visible; engine-enforced at read time.

**Why it's a good idea**:
- Pure declarative DDL — lives with the table, introspectable (`INFORMATION_SCHEMA` / `bq ls --row_access_policies`), live-mutable without app code or view proliferation.
- Principal lists are first-class in the DDL (GRANT TO supports user/group/domain/serviceAccount/allAuthenticatedUsers + federated workforce identities).
- Uses built-in `SESSION_USER()` for "current caller" without external context plumbing.
- Multiple policies compose cleanly (union for access); subqueries allow centralized lookup tables instead of N policies.
- Coexists with column-level security and higher IAM; "true" policy often paired for admins.
- No query change required — transparent to consumers.

**Quackapi mapping (server primitive)**: Directly informs `CREATE POLICY` (or `CREATE ROW ACCESS POLICY`) for web routes/endpoints/models. Policy would be a SQL predicate over request "claims" (analog of principal + attributes: user id, groups/roles from JWT, tenant, etc.). Applied automatically by the DuckDB query layer serving the route (e.g., `SELECT * FROM orders WHERE <policy_predicate>`). Multiple policies OR together. Introspectable as a system table. Informs `CREATE AUTH` indirectly (principals/claims source).

**Red-team: KEEP** — Core idea is excellent for quackapi. Engine-enforced, DDL-mutable, predicate-based authz over identity is a direct match. The GRANT TO + FILTER USING shape is clean and stealable. Subquery/lookup pattern is powerful for "policy as data". Minor bloat (100 limit, specific principal formats) can be generalized.

### 2. Column-level security via policy tags + Dynamic Data Masking (data policies)

**Exact DDL Grammar + Example**:
Policy tags are **not created via user SQL DDL** in `CREATE TABLE`. They are created via Data Catalog API/console (taxonomies + policytags). Attachment happens via schema JSON or bq update / console (or during table creation via bq mk/load with schema file). Data masking policies (rules) are also created via BigQuery Data Policy API / console, not pure user-facing CREATE DATA POLICY DDL in SQL.

Schema attachment example (JSON for bq or API):

```json
{
  "name": "email",
  "type": "STRING",
  "policyTags": {
    "names": ["projects/my-project/locations/us-central1/taxonomies/TAX_ID/policyTags/PII_EMAIL"]
  }
}
```

Data policies (masking rules) on a policy tag or directly on column:
- Created via API (`dataPolicies.create` with `DATA_MASKING_POLICY` + predefinedExpression like `SHA256`, `ALWAYS_NULL`, `DEFAULT_MASKING_VALUE`, `EMAIL_MASK`, or custom UDF).
- Attached by setting policy tag on column (or direct column data policy).
- Queriers with "Fine-Grained Reader" on the tag see raw; "Masked Reader" on the data policy see masked; others denied.
- Detach example (partial DDL support): `ALTER COLUMN col SET OPTIONS (data_policies=...)` or remove via schema update.

Predefined masking options (examples from docs): NULLIFY, default value, HASH (SHA-256 — joinable), RANDOM_HASH (per-query salt), EMAIL_MASK, FIRST/LAST N chars, DATE_YEAR, custom UDF.

Custom masking routine example (UDF used by data policy):

```sql
CREATE FUNCTION custom_mask(val STRING) RETURNS STRING AS ( ... );
-- Then referenced when creating the data policy (API)
```

**Concept (one line)**: Governance taxonomy (policy tags) + attached data policies define who sees raw vs masked vs nothing on tagged columns; enforced transparently by the query engine using IAM roles on tags/policies.

**Why it's a good idea**:
- Separation of classification (tags) from enforcement (data policies + IAM roles).
- Scale: one tag + policies protects many columns/tables.
- Multiple masking levels per tag (e.g., full for finance, hash for analysts, null for others).
- Dynamic (runtime) without ETL or per-query CASE.
- Joinability control (some masks preserve joins, random salt does not across queries).
- Custom UDF support for arbitrary logic.

**Quackapi mapping**: Informs `CREATE MODEL` / type validation + column policies, and `CREATE POLICY` for columns/fields. Could map policy tags to "sensitivity" tags or struct field annotations on models. Masking rules → pluggable transformation functions (or DuckDB expressions) applied to response structs/rows based on caller claims. `CREATE POLICY` could support column masks or field-level redaction predicates. Direct column data policies (vs tags) is a simpler path for quackapi. Informs response shaping / "CREATE MASK".

**Red-team: KEEP (core idea) / SKIP (implementation details)** — Tag-driven + multi-level masking + custom routines is worth stealing for governance + response transformation. But the external taxonomy + heavy IAM role hierarchy + API-heavy creation (no first-class SQL CREATE DATA POLICY) is enterprise bloat. SKIP the full taxonomy scaffolding for a web framework; KEEP the "attach mask function or expression to column/field based on caller attributes" + multiple rule tiers. Prefer inline or simple DDL attachment.

### 3. Authorized Views (and authorized datasets / materialized views)

**Exact DDL Grammar + Example**:
`CREATE VIEW` is standard:

```sql
CREATE [ OR REPLACE ] VIEW [project.]dataset.view_name
[OPTIONS(...)]
AS query;
```

Authorization is **not DDL** — performed via console ("Authorize views" on source dataset), API (`datasets.update` + setIamPolicy), or bq. You grant the *view* itself (identified specially) access to the source dataset.

Example flow:
1. `CREATE VIEW shared.github_analyst_view AS SELECT ... FROM source_dataset.github_contributors;`
2. On the *source* dataset: authorize the view (via Sharing > Authorize views or equivalent).
3. Grant viewers `bigquery.dataViewer` on the *view's* dataset + jobUser on project.

Authorized datasets allow authorizing a whole view dataset at once (better scaling).

**Concept (one line)**: A view runs with the privileges of its creator/owner (delegated access) rather than the querier, enabling safe exposure of filtered/aggregated subsets without granting base table access.

**Why it's a good idea**:
- Simple semantic layer + security boundary in one artifact.
- No row policy explosion for complex joins/aggregations.
- Materialized views can be authorized too (performance + security).
- Authorized datasets reduce per-view management.
- Works alongside row/column policies.

**Quackapi mapping**: Informs "authorized" or capability-carrying views/endpoints. For quackapi, `CREATE VIEW` (or a route backed by a view) that runs with elevated/defined privileges based on policy. Could be modeled as `CREATE ROUTE` or view that carries an implicit "run as" or attached policy. Useful for curated "safe" projections. In a pure-SQL web framework, the view definition itself can embed the filter, combined with row policies.

**Red-team: KEEP** — The delegation concept is good (view as a secure boundary with its own effective identity/rights). SKIP heavy side-band authorization steps (prefer everything in DDL or attached policy). In quackapi, prefer composing with `CREATE POLICY` on the view/route rather than separate "authorize" action.

**Data masking in views**: Authorized views do not automatically bypass column security on base tables; users still need appropriate tag roles for protected columns referenced (or mask at view level with CASE + group checks, but this is app-level, not engine data policy).

---

## Databricks Unity Catalog

### 1. ROW FILTER (table-level) + WITH ROW FILTER in CREATE TABLE

**Exact DDL Grammar + Example** (table-level manual application; also ABAC below):

First define UDF, then attach:

```sql
CREATE FUNCTION us_filter(region STRING)
  RETURNS BOOLEAN
  IF (IS_ACCOUNT_GROUP_MEMBER('admin'), true, region = 'US');

-- Attach on existing table
ALTER TABLE sales SET ROW FILTER us_filter ON (region);

-- Or at CREATE time
CREATE TABLE sales (region STRING, id INT)
WITH ROW FILTER us_filter ON (region);

-- Drop
ALTER TABLE sales DROP ROW FILTER;
```

The UDF is evaluated per row at query time; must return BOOLEAN; parameter types must match passed columns (or implicit cast with caveats).

Can take 0+ columns; additional columns for context.

**Concept (one line)**: Bind a boolean SQL UDF as a mandatory row filter on a table (or via ABAC); rows where it returns false are excluded before results are returned.

**Why it's a good idea**:
- UDFs are reusable, testable, versionable SQL (or wrappers over Python).
- Applied at the storage/query boundary (can't be bypassed by most operations).
- Supports context columns (e.g., pass email + user for "self or admin").
- Explicit in `CREATE TABLE` or `ALTER` — visible in metadata.
- Clear separation: policy logic in function, attachment declarative.

**Quackapi mapping**: Direct analogue to `CREATE POLICY` (row filter flavor). In quackapi, `CREATE POLICY name ON table_or_route ROW FILTER predicate_or_func TO claims...` or simply attach a predicate expression. The "WITH ROW FILTER" syntax in CREATE TABLE is nice for co-locating. Informs server primitive for automatic WHERE injection based on auth context (claims from JWT). UDF-style for complex logic.

**Red-team: KEEP** — Excellent. Simple, function-based, declarative attachment. Steal the attachment syntax and the "UDF or expression returns boolean" model. SKIP some Databricks-specific perf/ANSI cast quirks and limitations (e.g., can't apply to views in their model).

### 2. COLUMN MASK (table/column-level)

**Exact DDL Grammar + Example**:

```sql
CREATE FUNCTION ssn_mask(ssn STRING)
  RETURNS STRING
  CASE WHEN is_account_group_member('HumanResourceDept') THEN ssn ELSE '***-**-****' END;

-- At CREATE
CREATE TABLE users (name STRING, ssn STRING MASK ssn_mask);

-- Or ALTER
ALTER TABLE users ALTER COLUMN ssn SET MASK ssn_mask;

-- With additional columns (USING COLUMNS)
CREATE FUNCTION mask_address_by_country(address STRING, country STRING, group_suffix STRING DEFAULT '_address_viewers')
  RETURNS STRING
  IF (is_account_group_member(country || group_suffix), address, 'REDACTED');

CREATE TABLE customers (
  name STRING,
  address STRING MASK mask_address_by_country USING COLUMNS (country, '_address_viewers'),
  country STRING
);

ALTER TABLE ... ALTER COLUMN address SET MASK ... USING COLUMNS (...);

ALTER TABLE users ALTER COLUMN ssn DROP MASK;
```

Mask UDF takes the column as first arg (+ optional others via USING COLUMNS); return type must be compatible.

**Concept (one line)**: Attach a SQL UDF to a column that transforms the value seen by the caller at read time (conditional on identity/groups/other columns).

**Why it's a good idea**:
- Per-column, reusable masking logic.
- Can be conditional on other row values or caller identity without duplicating CASE everywhere.
- Preserves schema (same column name/type visible, different content).
- Works with row filters.
- USING COLUMNS for rich context without polluting every query.

**Quackapi mapping**: Informs field/column-level policies inside `CREATE MODEL` / struct types or `CREATE POLICY` column-mask variant. Response serialization could apply masks based on claims. `MASK func USING ...` is a great pattern for "redact this field using this logic and these other request/row attrs". Directly maps to response shaping in API handlers.

**Red-team: KEEP** — Strong idea. Steal attachment syntax and USING COLUMNS for context. In a web framework, this becomes response field transformers or policy-driven serialization. SKIP any Spark-specific type/ANSI issues.

### 3. CREATE POLICY (ABAC — Attribute-Based Access Control for row filters & column masks)

**Exact DDL Grammar + Example** (powerful centralized version):

```
CREATE [ OR REPLACE ] POLICY policy_name
ON { CATALOG catalog_name | SCHEMA schema_name | TABLE table_name }
[ COMMENT description ]
{ row_filter_body | column_mask_body }

row_filter_body:
  ROW FILTER function_name
  TO principal [, ...]
  [ EXCEPT principal [, ...] ]
  FOR TABLES
  [ WHEN condition ]
  [ MATCH COLUMNS condition [ [ AS ] alias ] [, ...] ]
  [ USING COLUMNS ( function_arg [, ...] ) ]

column_mask_body:
  COLUMN MASK function_name
  TO principal [, ...]
  [ EXCEPT principal [, ...] ]
  FOR TABLES
  [ WHEN condition ]
  [ MATCH COLUMNS condition [ [ AS ] alias ] [, ...] ]
  ON COLUMN alias
  [ USING COLUMNS ( function_arg [, ...] ) ]
```

Real examples:

```sql
-- Column mask on any table in catalog matching tag
CREATE FUNCTION ssn_to_last_nr (ssn STRING, nr INT) RETURNS STRING AS right(ssn, nr);

CREATE POLICY ssn_mask
  ON CATALOG employees
  COLUMN MASK ssn_to_last_nr
  TO 'All Users' EXCEPT 'HR admins'
  FOR TABLES
  MATCH COLUMNS has_tag('ssn') AS ssn
  ON COLUMN ssn
  USING COLUMNS (4);

-- Row filter on high-sensitivity tables in a schema, using geo column
CREATE FUNCTION non_eu_region (geo_region STRING) RETURNS BOOLEAN AS geo_region <> 'eu';

CREATE POLICY hide_eu_customers
  ON SCHEMA prod.customers
  COMMENT 'Hide European customers from sensitive tables'
  ROW FILTER non_eu_region
  TO analysts
  FOR TABLES
  WHEN has_tag_value('sensitivity', 'high')
  MATCH COLUMNS has_tag('geo_region') AS region
  USING COLUMNS (region);
```

Policies attach at catalog/schema/table scope and auto-apply to matching tagged tables/columns (governed tags). `has_tag` / `has_tag_value` in WHEN/MATCH.

**Concept (one line)**: Declare reusable row-filter or column-mask policies at container scope; they automatically apply to tables/columns matching tag conditions (WHEN / MATCH COLUMNS), scoped to principals — governance follows data labels, not objects.

**Why it's a good idea**:
- Shift from per-table config to "tag once, protect everywhere".
- Separation of duties: policy authors vs table owners (owners can't easily bypass high-level policies).
- Scales to hundreds/thousands of tables without repetitive ALTERs.
- WHEN conditions + governed tags enable attribute-based rules.
- Still uses the same UDFs for logic.
- Introspectable (`SHOW POLICIES`, `DESCRIBE POLICY`).

**Quackapi mapping**: Goldmine for `CREATE POLICY`. Steal the scoped attachment (`ON CATALOG/SCHEMA/TABLE` or "ON ROUTE / MODEL / *"), principal TO/EXCEPT, WHEN conditions (on "tags" or attributes), MATCH for fields/columns, USING COLUMNS. In quackapi: policies could live on "apps", "route groups", or models and auto-apply based on model tags or field tags + caller claims. Perfect for "govern once". Directly informs advanced `CREATE POLICY` + tagging of routes/models/fields.

**Red-team: KEEP** — One of the best ideas here. Tag/policy-driven application is transformative for maintainability. The CREATE POLICY grammar is expressive yet declarative. Steal the structure (scope + filter/mask + principals + conditions + column matching). SKIP full "governed tags" bureaucracy if too heavy; a lightweight tag system on tables/fields/routes would suffice. Excellent for multi-tenant or sensitivity-based web APIs.

### 4. CREATE SHARE (for Delta Sharing / OpenSharing)

**Exact DDL Grammar + Example**:

```
CREATE SHARE [ IF NOT EXISTS ] share_name
    [ COMMENT comment ]
```

Then populate via `ALTER SHARE`:

```sql
CREATE SHARE IF NOT EXISTS customer_share COMMENT 'This is customer share';

ALTER SHARE customer_share ADD TABLE my_catalog.default.my_table;

-- Partitioned / aliased / recipient-property filtered shares
ALTER SHARE share_name
ADD TABLE inventory
  PARTITION (year = "2021"),
            (year = "2020", month = "Dec");

-- Dynamic view for recipient-specific filtering (using CURRENT_RECIPIENT())
CREATE VIEW ... AS SELECT ... WHERE country = CURRENT_RECIPIENT('country');
```

Shares can include tables (with/without history), views (including dynamic), volumes, models, etc. Recipients defined separately (`CREATE RECIPIENT`); access granted to recipients on shares.

Dynamic views inside shares use `CURRENT_RECIPIENT('prop')` for per-recipient row/col logic.

**Concept (one line)**: A named, versioned container of data assets (tables/views/...) with explicit recipient grants and optional partition/alias/dynamic logic for controlled external sharing.

**Why it's a good idea**:
- First-class object for "what I share with whom" — auditable, mutable via DDL.
- Supports fine-grained sharing (partitions, dynamic redaction via views using recipient properties).
- Decouples asset definition from access grants.
- Enables secure cross-org / cross-cloud sharing with protocol (Delta Sharing).
- History sharing, aliases, etc. are declarative.

**Quackapi mapping**: Informs external exposure / "public" or partner API surfaces. `CREATE SHARE` or `CREATE EXPOSURE` / `CREATE CORS_POLICY` + recipients. Could map to publishing routes/models to external consumers with scoped views or policies. Recipient properties + `CURRENT_RECIPIENT()` is a nice pattern for "per-tenant or per-partner" filters without per-recipient objects. Useful for "CREATE JOB" or partner-specific endpoints. In web terms: controlled "share" of API surface with authz.

**Red-team: KEEP (sharing abstraction) / PARTIAL** — The named share + ALTER + recipient properties + dynamic views inside is good for governed externalization. For quackapi (internal web framework), it maps less directly unless you want first-class "partner portals" or multi-tenant exposure. SKIP heavy Delta Sharing protocol specifics; KEEP the declarative container + properties-driven filtering idea for "CREATE EXPOSURE" or per-client policies.

### 5. GRANT (privileges on securables)

**Exact DDL Grammar + Examples** (from concepts and related):

```
GRANT privilege [, ...]
ON { CATALOG name | SCHEMA name | TABLE name | VIEW ... | FUNCTION ... | SHARE ... | ... }
TO principal [, ...];
```

Examples:

```sql
GRANT USE CATALOG, USE SCHEMA, SELECT ON CATALOG sales TO finance_team;

GRANT USE CATALOG, USE SCHEMA, CREATE TABLE ON CATALOG analytics TO data_engineers;

-- On a specific table
GRANT SELECT, MODIFY ON TABLE prod.db.transactions TO analysts;
```

Privileges are granular (SELECT, MODIFY, EXECUTE, etc.) + usage (USE CATALOG / USE SCHEMA required). Inheritance from containers. Ownership implies broad rights. `REVOKE`, `SHOW GRANTS`, `DENY` patterns exist.

Also special: `MANAGE`, `BROWSE`, `ALL PRIVILEGES` (with caveats).

**Concept (one line)**: Declarative DCL to grant fine-grained privileges on hierarchical securables (catalog/schema/table/...); usage privileges gate access; inheritance simplifies management.

**Why it's a good idea**:
- SQL-standard GRANT as the source of truth for access.
- Hierarchical + inheritance reduces repetition.
- Usage privileges prevent accidental over-granting by lower owners.
- Clear ownership model with MANAGE as delegation.
- Introspectable (`SHOW GRANTS`).

**Quackapi mapping**: Informs `GRANT` / authz layer for routes, models, jobs. Even in a simple framework, `GRANT SELECT ON route_or_model TO role` or claims-based. Pairs with `CREATE POLICY` (policies can be the enforcement; GRANT the assignment). Hierarchical (app / route-group / route) is worth copying. For JWT claims, map "principals" to roles/groups in claims.

**Red-team: KEEP** — Standard, necessary, introspectable DCL is table stakes. Steal hierarchy + usage gating idea. For quackapi, keep it lightweight (no 3-level catalog unless wanted). SKIP complex ALL PRIVILEGES semantics or metastore-level if not relevant.

### 6. Dynamic Views (for row/column security)

**Exact DDL Grammar + Example**:

Standard `CREATE VIEW`, with security logic inside:

```sql
CREATE VIEW sales_redacted AS
SELECT
  user_id,
  country,
  product,
  total,
  CASE WHEN is_account_group_member('auditors') THEN email ELSE 'REDACTED' END AS email
FROM sales_raw
WHERE
  CASE
    WHEN is_account_group_member('managers') THEN TRUE
    ELSE total <= 1000000
  END;

-- Or using recipient properties for shares
CREATE VIEW ... AS
SELECT ...
WHERE country = CURRENT_RECIPIENT('country');
```

**Concept (one line)**: Express row filters and column masks as ordinary view logic using identity/group functions (`is_account_group_member`, `current_user`, `CURRENT_RECIPIENT`); consumers query the view instead of base tables.

**Why it's a good idea**:
- No new syntax for simple cases — just SQL.
- Flexible: joins, complex expressions, regex masking, etc.
- Works for curated "semantic" layers.
- Can be shared via CREATE SHARE.
- Easy to reason about (it's a view).

**Quackapi mapping**: Informs using views or `CREATE VIEW` + policies for safe endpoints. In quackapi, a "route" can be a view definition that embeds claim-based CASE/WHERE. Complements (or simpler alternative to) row/column policies. Good for "CREATE MODEL" projections too.

**Red-team: KEEP (as complement)** — Simple and powerful. SKIP as *only* mechanism if you want engine-enforced policies that can't be bypassed by querying the base. Best used together with row/column filters (note: some restrictions on sharing tables that have filters + dynamic views using member funcs).

---

## Other Relevant / Related Patterns Observed

- **CREATE FUNCTION** (both platforms): Central for policy logic (filters, masks, custom masking routines). Reusable, introspectable, typed. **KEEP** strongly — quackapi should allow `CREATE FUNCTION` for policies.
- **BigQuery authorized datasets**: Authorize a *dataset* of views at once (scaling authorized views). Analog: group policies or expose route groups.
- **Databricks ABAC tags + has_tag()**: Governed metadata drives policy application. **KEEP** lightweight version for quackapi models/routes/fields.
- **No native CREATE RATE LIMIT / CORS / JOB in these DDLs**: BigQuery/Databricks are analytical platforms, not web servers. Quotas exist as service limits / reservations (BigQuery has capacity commitments). Databricks has jobs but via jobs API / workflows, not pure CREATE CRON DDL in core UC (though Delta Live Tables / workflows have scheduling). SKIP direct copies; instead extract the *declarative policy* spirit for rate limiting (e.g., `CREATE RATE LIMIT policy ON route ...` using simple counters or table-backed).
- **No CREATE AUTH / JWT / SECRET in focus**: Both rely on external identity providers (IAM, account groups, SSO). Session functions (`SESSION_USER()`, `is_account_group_member()`) consume the authenticated context. For quackapi: `CREATE AUTH` would be the place to declare JWT verification (crypto ext + CREATE SECRET for keys) + claim extraction into session context usable by policies.
- **Ownership + MANAGE + inheritance**: Strong governance primitives. Worth stealing lightweight ownership/transfer + "manage" delegation for routes/policies.
- **Introspection everywhere**: `INFORMATION_SCHEMA`, `SHOW`, `DESCRIBE`, `bq ls --row_access_policies`, `SHOW POLICIES`. **KEEP** — quackapi's power is tables-as-control-plane; every CREATE-* should be queryable.

**CREATE MODEL / TYPE angle**: BigQuery has `CREATE MODEL` (ML). Databricks has models as securables. Not deeply covered here, but struct types + policies on fields map to "CREATE TYPE/MODEL with attached policies".

---

## Ranked Shortlist: Top 5 Ideas Worth Stealing for quackapi

1. **CREATE POLICY (Databricks ABAC style) + table/column attachment with conditions** (BigQuery row access + Databricks row filter/column mask + ABAC)  
   Scope attachment (ON ...), TO/EXCEPT principals, WHEN/MATCH conditions on tags/attrs, reusable filter/mask UDFs or expressions. The single best pattern for scalable, declarative, engine-enforced authorization that lives in DDL. Directly maps to quackapi `CREATE POLICY`.

2. **Row access policies as first-class DDL with GRANT TO + FILTER USING predicate** (BigQuery)  
   Explicit, live-mutable, multi-policy composition (OR), `SESSION_USER()`-style caller binding, subquery support. Clean grammar that makes authz visible and queryable. Core for `CREATE POLICY` / route protection.

3. **Column/field masking via attached functions with USING COLUMNS context** (both, esp. Databricks)  
   `MASK func [USING ...]` or data policy rules. Perfect for response redaction without per-handler code. Pairs with `CREATE MODEL` / struct field policies and output shaping.

4. **Hierarchical GRANT + usage/guard privileges + inheritance** (Databricks UC)  
   `GRANT ... ON CATALOG/SCHEMA/... TO ...`; USE CATALOG/SCHEMA gating; container inheritance. Teaches how to structure privilege assignment so lower-level owners can't exfiltrate. Adapt to route groups / apps in quackapi.

5. **Policy / security logic expressed as (or attached to) reusable SQL functions/UDFs + dynamic views** (both)  
   `CREATE FUNCTION` for filters/masks/custom logic; views for curated secure projections (with identity functions inside). Reusability + testability + no duplication. Informs policy bodies, `CREATE AUTH` claim helpers, and safe "views" of models.

**Honorable mentions (strong but secondary)**: 
- Named SHARE + recipient properties + CURRENT_RECIPIENT() for controlled externalization / partner views (informs exposure/CORS-like primitives).
- Data policies with tiered masking rules (full / masked / none) attached to classifications — generalize to claim-based response tiers.
- Full introspection of policies/grants/filters as queryable metadata.

**Ideas largely SKIPped**:
- Heavy external taxonomy + IAM role hierarchies for policy tags (too much for a web framework; prefer inline tags or simple labels + policies).
- Side-band "authorize view" steps (fold into policy attachment or ownership).
- Full enterprise quota/reservation DDL (use simple table-backed rate limit policies instead).
- Delta-sharing protocol specifics or metastore-level grants.

This report prioritizes patterns that make server behavior **declarative, SQL-expressible, engine-enforced, introspectable, and runtime-mutable** — exactly quackapi's differentiator.

---

**Citations / References** (key pages used):
- BigQuery row-level: https://cloud.google.com/bigquery/docs/row-level-security-intro , managing docs, DDL reference (create_row_access_policy_statement).
- BigQuery column / masking: https://cloud.google.com/bigquery/docs/column-level-security , column-data-masking-intro , column-data-masking.
- Authorized views: https://cloud.google.com/bigquery/docs/authorized-views , create-authorized-views.
- Databricks row filters / masks: https://docs.databricks.com/aws/en/data-governance/unity-catalog/filters-and-masks/ and /manually-apply .
- Databricks CREATE POLICY (ABAC): https://docs.databricks.com/aws/en/sql/language-manual/sql-ref-syntax-ddl-create-policy and ABAC policies page.
- CREATE SHARE: https://docs.databricks.com/aws/en/sql/language-manual/sql-ref-syntax-ddl-create-share , opensharing/create-share.
- Permissions/GRANT: Unity Catalog permissions model concepts + privileges reference.
- Dynamic views: Databricks views/dynamic docs.

All syntax and examples cross-checked against the live/official docs pages fetched in research. Adapt conservatively — steal the spirit and clean shapes, not the enterprise surface area.