# Validation doctrine — surpass FastAPI + Pydantic, not imitate FastAPI

**Status:** RULED (Alok, 2026-07-10). This supersedes "FastAPI parity" as the
validation design compass. FastAPI agreement is scoreboard *evidence*; the
*principle* below is the compass. Reframes BACKLOG "conformance coercion policy".

## The principle

**The declared SQL type is the contract. Validation is membership in that type,
parsed from its canonical string forms. Reject everything else.**

Pydantic exists because the web layer is typeless and the real type system —
the database's — sits stranded behind an ORM. quackapi's params are declared in
SQL types in the layer the data lives in. There is no second schema, so there
is no drift and no bridge (#214 dissolved). Validation is not a copy of the
schema; it IS the schema.

Corollary: DuckDB's *cast* operator is not the validator. Casts implement
ingestion semantics ('1.5'::INT → 2, '1e2'::INT → 100 — tuned for messy CSVs).
Validators state the membership predicate explicitly (strict-int gate,
framework.sql ~1624). The database still validates — we tell it the exact
question, same as a CHECK constraint.

## Canonical forms (rulings)

| Declared type | Accepted string forms | Rejected (422) |
|---|---|---|
| INT/BIGINT | optional sign + digits (`123`, `-7`) | `1.5`, `1e2`, `+ 1`, ` 1 `, `0x10` |
| BOOL | `true`, `false`, `1`, `0` (**ruled 2026-07-10**) | `yes`, `no`, `on`, `off`, `TRUE`? — case: accept case-insensitive true/false (canonical JSON is lowercase; query params get ci) |
| DOUBLE/DECIMAL | standard numeric literal incl. `.`/exponent | `NaN`/`Infinity` in JSON bodies (invalid JSON anyway), empty string |
| TEXT | any string | — (constraints via CHECK predicates) |
| DATE/TIMESTAMP | ISO 8601 | lenient partial forms DuckDB's cast tolerates |

Where Pydantic is *more lenient* than the principle (BOOL `"yes"`, string
`"1_000"` ints), quackapi rejects — **stricter than Pydantic on purpose**,
documented as divergence, not a bug. The 13 parked strict-vs-coerce cases are
to be audited against this table mechanically; only genuine ambiguities go
back to Alok.

## Beyond shape: truth validation (the orthogonal surpass)

Pydantic validates *shape*; it structurally cannot validate *truth* (no data in
the loop). quackapi validates both in one layer:

- referential: `CHECK (EXISTS (SELECT 1 FROM customers WHERE id = {customer_id}))`
- uniqueness / cross-record: one EXISTS, same transaction
- cross-field: any SQL predicate over the param set

## NULL trinity (the Pydantic wound, solved grammatically)

Pydantic v1 conflated nullable with optional (`Optional[int]`); v2 broke compat
to fix it and still needs `model_fields_set` bookkeeping to remember what the
client sent. SQL grammar had the two axes orthogonal all along, and DuckDB's
extraction primitive natively distinguishes the runtime trinity (verified:
`json_extract('{}','$.k')` → SQL NULL; `json_extract('{"k":null}','$.k')` →
JSON null, NOT SQL NULL; value → value).

Declaration matrix (param DDL = SQL column grammar, zero new concepts):

| Declaration | Key absent | `null` sent | Value sent |
|---|---|---|---|
| `(name TEXT)` | 422 field required | 422 null not allowed | ok |
| `(name TEXT NULL)` | 422 field required | ok (SQL NULL) | ok |
| `(name TEXT DEFAULT 'x')` | ok → 'x' | 422 null not allowed | ok |
| `(name TEXT NULL DEFAULT NULL)` | ok → NULL | ok (SQL NULL) | ok |

Runtime surface: every body param additionally exposes `{name__provided}`
(BOOLEAN: key present in request). PATCH partial update — FastAPI's
`exclude_unset` dance — becomes one expression:

```sql
UPDATE users SET name = CASE WHEN {name__provided} THEN {name} ELSE name END
WHERE id = {id} RETURNING to_json(users) AS body;
```

`{"name": null}` clears the field; `{}` leaves it untouched. 422 errors
distinguish `missing` (absent, required) from `null_not_allowed` (present null,
non-nullable) as distinct machine-readable codes — more precise than Pydantic.

Honest edge: JSON-null vs SQL-NULL is a known analytics-view footgun (COALESCE
does not defend against JSON null). The same distinction is what makes the
trinity possible — the framework code guards every json_extract site with
explicit trinity handling; tests must cover all three states per nullable
param.

## Status

- Strict INT gate: SHIPPED (framework.sql ~1624).
- BOOL 1/0/true/false ruling: this doc; implementation audit pending.
- NULL trinity (`NULL`/`DEFAULT` param grammar + `{p__provided}` + split 422
  codes): SPEC'D HERE, not yet built — backlog item, after OAuth (#335).
- 13 conformance cases: audit mechanically against the canonical-forms table;
  escalate only true ambiguities.
