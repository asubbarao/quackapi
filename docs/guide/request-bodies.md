# Request bodies

Path, query, header, cookie, and body fields all bind into the same **`$name`** namespace. You do not declare a separate “body model class” — you use `$name` in SQL (and optionally a JSON Schema).

All examples run against `build/release/duckdb -unsigned` with `LOAD quackapi;`.

---

## JSON body

Fields of a JSON object become parameters when the client sends `Content-Type: application/json`.

```sql
CREATE ROUTE create_user POST '/users' STATUS 201 AS
SELECT $name::VARCHAR AS name, $age::INTEGER AS age;
```

```sh
curl -X POST http://127.0.0.1:8000/users \
  -H 'Content-Type: application/json' \
  --data-binary '{"name":"dave","age":35}'
# [{"name":"dave","age":35}]
# HTTP 201
```

### Malformed JSON → 422

```sh
curl -X POST http://127.0.0.1:8000/users \
  -H 'Content-Type: application/json' \
  --data-binary '{not json}'
# {"detail":[{"loc":["body"],"msg":"JSON decode error","type":"json_invalid"}]}
# HTTP 422
```

### Wrong Content-Type → 422

```sh
curl -X POST http://127.0.0.1:8000/users \
  -H 'Content-Type: text/plain' \
  --data-binary '{"name":"x","age":5}'
# {"detail":[… "loc":["body"] … "type":"model_attributes_type" …]}
# HTTP 422
```

### Body field type error

```sh
curl -X POST http://127.0.0.1:8000/users \
  -H 'Content-Type: application/json' \
  --data-binary '{"name":"x","age":"nope"}'
# loc=["body","age"], type=type_error
# HTTP 422
```

### Query wins when both are present

If the same name is in the query string and the JSON body, the **query value wins**:

```sh
curl -X POST 'http://127.0.0.1:8000/users?name=fromq&age=9' \
  -H 'Content-Type: application/json' \
  --data-binary '{"name":"fromb","age":99}'
# [{"name":"fromq","age":9}]
```

---

## Raw body (`$body`)

When the handler references `$body`, the raw payload string is available:

```sql
CREATE ROUTE echo_body POST '/echo-body' AS
SELECT $body::VARCHAR AS body;
```

```sh
curl -X POST http://127.0.0.1:8000/echo-body \
  -H 'Content-Type: application/json' \
  --data-binary '{"k":1}'
# body field contains the raw JSON text
```

---

## Form urlencoded

`Content-Type: application/x-www-form-urlencoded` binds form fields the same way:

```sql
CREATE ROUTE form_submit POST '/form-submit' AS
SELECT $name::VARCHAR AS name, $age::INTEGER AS age;
```

```sh
curl -X POST http://127.0.0.1:8000/form-submit \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data-binary 'name=zed&age=31'
# [{"name":"zed","age":31}]
# HTTP 200
```

`+` and `%20` decode as spaces.

---

## Multipart file upload

Multipart fields and files bind by part name. File content is `$file` (or the part name). Filename helpers:

- `$file_filename` — filename for the part named `file`
- `$filename` — convenience alias used in common handlers

```sql
CREATE ROUTE upload POST '/upload' AS
SELECT $file::VARCHAR AS content, $file_filename::VARCHAR AS filename;
```

```sh
# multipart boundary=testbnd, part name=file, filename=test.txt, body=hello
curl -X POST http://127.0.0.1:8000/upload \
  -H 'Content-Type: multipart/form-data; boundary=testbnd' \
  --data-binary $'--testbnd\r\nContent-Disposition: form-data; name="file"; filename="test.txt"\r\n\r\nhello\r\n--testbnd--\r\n'
# [{"content":"hello","filename":"test.txt"}]
```

Fields + file:

```sql
CREATE ROUTE upload_fields POST '/upload-fields' AS
SELECT $title::VARCHAR AS title,
       $file::VARCHAR AS content,
       $filename::VARCHAR AS filename;
```

---

## BODY SCHEMA (JSON Schema validation)

Add `BODY SCHEMA '<json-schema>'` to validate the JSON object **before** the handler runs. This uses DuckDB’s `json_schema` machinery under the hood.

```sql
CREATE ROUTE create_user POST '/users' STATUS 201
  BODY SCHEMA '{"type":"object","required":["name","age"],"properties":{"name":{"type":"string"},"age":{"type":"integer"}},"additionalProperties":false}'
  AS
SELECT $name::VARCHAR AS name, $age::INTEGER AS age;
```

```sh
curl -X POST http://127.0.0.1:8000/users \
  -H 'Content-Type: application/json' \
  --data-binary '{"name":"ada","age":42}'
# [{"name":"ada","age":42}]
# HTTP 201

curl -X POST http://127.0.0.1:8000/users \
  -H 'Content-Type: application/json' \
  --data-binary '{"name":"ada"}'
# HTTP 422 — loc includes body, type=value_error (required property 'age')
```

`BODY SCHEMA` can appear before or after `PARAM` clauses. Quote the schema as a SQL string (`''` to escape a single quote inside).

---

## Body size limit

Request bodies larger than **8 MiB** are rejected with **413**.

---

## Mutation pattern (INSERT … RETURNING)

Handlers are ordinary SQL. Mutations are `INSERT` / `UPDATE` / `DELETE` with `RETURNING`:

```sql
CREATE TABLE decisions (
  suggestion_id INTEGER,
  status VARCHAR,
  actor VARCHAR
);

CREATE ROUTE decide POST '/api/suggestions/:id/decision' STATUS 201
  PARAM status VARCHAR
  PARAM actor VARCHAR DEFAULT 'reviewer'
  AS
INSERT INTO decisions BY NAME
SELECT $id::INTEGER AS suggestion_id,
       $status AS status,
       $actor AS actor
RETURNING *;
```

```sh
curl -X POST http://127.0.0.1:8000/api/suggestions/1/decision \
  -H 'Content-Type: application/json' \
  -d '{"status":"accepted"}'
```

---

## Next

- [Headers, cookies, redirects, status codes](headers-cookies-redirects.md)  
- [Auth](auth.md)
