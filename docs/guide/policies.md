# Row access & masking policies

Policies are a security leapfrog over vanilla FastAPI: **claims-keyed row filters** and **column masks** expressed in SQL, bound to tables, enforced on HTTP handlers that touch those tables.

All examples run against `build/release/duckdb -unsigned` with `LOAD quackapi;`.

---

## The idea

1. Authenticate with JWT (or API key).  
2. Claims bind as `$claims_tenant`, `$claims_role`, `$claims_sub`, …  
3. A **row access policy** is a boolean expression over table columns + claims.  
4. A **masking policy** rewrites a column value (placeholder `val` = original).  
5. `ALTER TABLE` binds policies to tables/columns.  
6. Routes that `SELECT` from bound tables get filters/masks automatically.

Unauthenticated access to a policied table fails **closed** (HTTP **403**).

---

## Row access policy

```sql
CREATE TABLE pol_orders (
  id INTEGER,
  tenant_id VARCHAR,
  amount INTEGER
);

INSERT INTO pol_orders VALUES
  (1, 'acme', 100),
  (2, 'beta', 200),
  (3, 'acme', 150);

CREATE AUTH jwt_pol AS JWT ( SECRET 'policy-http-secret' );

CREATE ROW ACCESS POLICY tenant_isolation
  AS (tenant_id VARCHAR) RETURNS BOOLEAN
  USING (tenant_id = $claims_tenant OR $claims_role = 'admin');

ALTER TABLE pol_orders
  ADD ROW ACCESS POLICY tenant_isolation ON (tenant_id);

CREATE ROUTE list_orders GET '/orders' REQUIRE jwt_pol AS
SELECT id, tenant_id, amount FROM pol_orders ORDER BY id;
```

JWT payload for tenant `acme`, role `user` → only acme rows:

```sh
curl http://127.0.0.1:8000/orders -H "Authorization: Bearer $JWT_ACME"
# [{"id":1,"tenant_id":"acme","amount":100},{"id":3,"tenant_id":"acme","amount":150}]
# HTTP 200
```

Admin role (`$claims_role = 'admin'`) sees all tenants.  
Missing JWT on a `REQUIRE` route → **401** (auth runs before policy).  
Public route on a policied table without claims → **403** “Policy denies…”.

---

## Masking policy

```sql
CREATE TABLE pol_users (
  id INTEGER,
  email VARCHAR,
  tenant_id VARCHAR
);

CREATE MASKING POLICY mask_email ON VARCHAR
  USING (CASE WHEN $claims_role = 'admin' THEN val ELSE '***' END);

ALTER TABLE pol_users
  ADD ROW ACCESS POLICY tenant_isolation ON (tenant_id);

ALTER TABLE pol_users
  MODIFY COLUMN email SET MASKING POLICY mask_email;

CREATE ROUTE list_users GET '/users' REQUIRE jwt_pol AS
SELECT id, email, tenant_id FROM pol_users ORDER BY id;
```

| Role | `email` column |
|------|----------------|
| `user` | `***` |
| `admin` | real address |

`val` is the original column value inside the masking expression.

---

## Inspect

```sql
SELECT name, kind, signature, expression, bound_table, bound_columns
FROM quackapi_policies();
```

| kind | Meaning |
|------|---------|
| `ROW_ACCESS` | Row filter |
| `MASKING` | Column rewrite |

---

## Grammar

```sql
CREATE [OR REPLACE] ROW ACCESS POLICY <name>
  AS (<col> [<type>] [, ...]) RETURNS BOOLEAN
  USING (<boolean expr>);

CREATE [OR REPLACE] MASKING POLICY <name>
  ON <type>
  USING (<expr using val and $claims_*>);

ALTER TABLE <t> ADD ROW ACCESS POLICY <name> ON (<cols>);
ALTER TABLE <t> MODIFY COLUMN <col> SET MASKING POLICY <name>;

DROP ROW ACCESS POLICY <name>;
DROP MASKING POLICY <name>;
```

Names must be unique across both policy kinds.

---

## Design notes

- Policies are **helpers**, not a full multi-tenant RBAC product.  
- Prefer JWT claims you control (`tenant`, `role`) over trusting query params.  
- Combine with [CREATE GROUP](groups.md) so every `/api/v1/*` route requires the same auth scheme.

---

## Next

- [Static files](static-files.md)  
- [OpenAPI](openapi.md)
