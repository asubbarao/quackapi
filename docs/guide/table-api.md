# CREATE API FOR TABLE — instant read CRUD

One statement expands a table (or view) into **two GET routes**: list all rows, and get one row by key.

Read-only in this version — no POST/PUT/DELETE scaffold. Generated routes are ordinary registry rows (`DROP ROUTE` works on each).

All examples run against `build/release/duckdb -unsigned` with `LOAD quackapi;`.

---

## Minimal

```sql
CREATE TABLE cases AS
SELECT 1 AS id, '24-000117' AS case_no;

CREATE API FOR TABLE cases;
-- registers:
--   GET /cases        → SELECT * FROM "cases"
--   GET /cases/:id    → SELECT * FROM "cases" WHERE "id" = $id
```

```sh
curl http://127.0.0.1:8000/cases
# [{"id":1,"case_no":"24-000117"}]

curl http://127.0.0.1:8000/cases/1
# [{"id":1,"case_no":"24-000117"}]
```

Inspect:

```sql
SELECT name, method, pattern FROM quackapi_routes() ORDER BY name;
-- cases_get   GET  /cases/:id
-- cases_list  GET  /cases
```

---

## Custom path and key

```sql
CREATE TABLE documents AS
SELECT 1 AS doc_id, 'report.pdf' AS filename;

CREATE API FOR TABLE documents AT '/api/documents' KEY 'doc_id';
-- GET /api/documents
-- GET /api/documents/:doc_id
```

Defaults:

| Clause | Default |
|--------|---------|
| `AT` | `'/<table>'` |
| `KEY` | `'id'` |

`AT` must be a quoted path starting with `/`.  
`KEY` must be a bare identifier (letters, digits, underscore) — it becomes the path param name.

---

## OR REPLACE and DROP

```sql
CREATE OR REPLACE API FOR TABLE documents AT '/api/documents' KEY 'doc_id';

DROP ROUTE documents_list;
DROP ROUTE documents_get;
```

---

## When to use this vs hand-written routes

| Use `CREATE API FOR TABLE` when… | Write `CREATE ROUTE` when… |
|----------------------------------|----------------------------|
| You want a quick list + get | You need filters, joins, writes |
| Columns are already the API | You need auth, body, custom status |
| Prototyping a table | Production shape differs from the table |

You can mix both: scaffold with `CREATE API FOR TABLE`, then add custom routes alongside.

---

## Next

- [CREATE GROUP](groups.md) — put table routes under `/api/v1` with auth  
- [Policies](policies.md) — tenant filters on the same tables
