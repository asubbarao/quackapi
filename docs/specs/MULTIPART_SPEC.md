# MULTIPART_SPEC — multipart/form-data for quackapi

**Status:** Implemented (oracle + C mirror).  
**Date:** 2026-07-02  
**Follows:** R1_REQUEST_SURFACE_SPEC.md pattern (SQL oracle contract; C mirror byte-identical).

---

## 1. Scope

Adds `multipart/form-data` request body parsing to `handle_request`. Non-file parts
map to route params identically to form-urlencoded fields (location='body'). File parts
map to two params per declared `type='file'` field: the declared name holds the file
content, and a companion auto-extracted name `<name>__filename` holds the
Content-Disposition filename (or empty string if absent).

FastAPI uses `UploadFile` objects; quackapi uses `type='file'` in `param_schema` because
there are no Python object references — everything is a VARCHAR. The param value IS the
file content as a VARCHAR string.

---

## 2. Content-Type detection

Multipart is active when:

```
lower(coalesce(content-type header, ''))
  STARTS WITH 'multipart/form-data'
  AND contains the substring 'boundary='
```

Detection uses `starts_with` and `instr`/`strpos` only (no LIKE, no regex).

Boundary extraction:
- Look for `boundary=` (case-insensitive) in the Content-Type value.
- The boundary value is everything after `boundary=` to the next `;` or end-of-string.
- Quoted boundary (`boundary="foo bar"`) is supported: strip surrounding double-quotes.
- The delimiter used in the body is `--<boundary>`.
- The closing delimiter is `--<boundary>--`.

## 3. Part parsing rules (CRLF discipline)

The body arriving at the SQL oracle is a VARCHAR. The C layer passes raw body bytes.
CRLF (`\r\n`) is the line separator per RFC 2046.

Parse pipeline:
1. Split body by `\r\n--<boundary>` to get parts. Trim leading `\r\n` from the first
   part if present.
2. Discard the preamble (empty string before first delimiter).
3. Discard any part equal to `--` (the terminal delimiter suffix).
4. For each remaining part:
   a. Split on the first `\r\n\r\n` to separate headers from content.
   b. Parse `Content-Disposition` from the part header block: extract `name=` value
      (quoted or unquoted) and optional `filename=` value.
   c. Part content is everything after `\r\n\r\n`, with the trailing `\r\n` stripped
      if present (added by the boundary split).
   d. Optionally parse per-part `Content-Type` header (stored but not used for routing
      in v1 — available for route handler inspection if needed).

## 4. Param mapping

### Non-file parts (no filename in Content-Disposition)

The part content string becomes the value for the param named by `name=` in
Content-Disposition. These feed the existing `val_str` → try_cast → constraint →
required pipeline exactly as form-urlencoded fields do (location='body').

### File parts (filename present in Content-Disposition)

A route declares a file param with `type='file'` in param_schema. When a multipart
part with `filename=` is present:
- The declared param name receives the file content as a VARCHAR string.
- A synthetic companion param `<name>__filename` is auto-populated with the filename
  string. Routes can inspect this (it is not declared in param_schema — it is passed
  through the handler template substitution machinery like any other param).

`type='file'` params:
- Are required/optional via the `required` flag as usual.
- Do NOT do try_cast (the value is raw content; no type coercion applies).
- Constraints (le/ge) do not apply (type='file' skips constraint checks).
- A missing file part (no matching multipart part and required=true) → 422 with
  `{"type":"missing","loc":["body","<name>"],"msg":"Field required"}`.

### Malformed multipart → 422

If `Content-Type` advertises multipart/form-data but the body does not contain the
declared boundary at all, return 422 with:
```json
{"detail":[{"type":"multipart_parse","loc":["body"],"msg":"Malformed multipart body: boundary not found"}]}
```

This is a quackapi extension (no direct FastAPI analog; FastAPI raises a 400 from
Starlette). The `loc` is `["body"]` (no field name) since the error is structural.

---

## 5. 422 shapes

All 422 errors match the FastAPI loc/msg/type shape where a direct analog exists:

| Scenario | type | loc | msg |
|----------|------|-----|-----|
| Required file missing | `missing` | `["body","<name>"]` | `Field required` |
| Required text field missing | `missing` | `["body","<name>"]` | `Field required` |
| Int field with non-int value | `int_parsing` | `["body","<name>"]` | `Input should be a valid integer...` |
| Malformed multipart (no boundary) | `multipart_parse` | `["body"]` | `Malformed multipart body: boundary not found` |

The `multipart_parse` error type has no direct FastAPI equivalent (FastAPI uses HTTP
400 from Starlette's multipart parser). We use 422 to stay consistent with the
framework's single-422-path for all validation failures.

---

## 6. Binary safety — EXPLICIT LIMITATION

**DuckDB VARCHAR is NOT null-byte safe.** A null byte (`\x00`) in a file part content
will truncate the string at that byte in most DuckDB operations.

**V1 restriction: text-safe payloads only.** This means:

- Text files (UTF-8, ASCII), CSV, JSON, SQL, plain source code: safe.
- Binary files (images, PDFs, executable, zip): NOT supported. Content will be
  silently truncated at any null byte, producing corrupted data without an error.

This is NOT a silent claim of binary support. Routes accepting `type='file'` params
MUST document that they only handle text-safe content. A future v2 can address this
by passing binary content as base64 (encoding in C, decoding in handler) or via a
DuckDB BLOB type if the extension pipeline supports it.

No error is emitted for binary content (we cannot detect null bytes in SQL VARCHAR
without OS-level byte inspection). The caller is responsible.

---

## 7. Route declaration example

```sql
-- Register an upload route
INSERT INTO routes SELECT * FROM register_route(
  'upload',
  'POST',
  '/upload',
  'SELECT to_json({''filename'': {file__filename}, ''size'': len({file}), ''preview'': substr({file},1,80)}) AS body',
  'dynamic',
  'File upload demo',
  200
);
-- Declare the file param (type=file, required=true)
INSERT INTO param_schema (route_id, name, location, type, required, constraint_json)
VALUES ('upload', 'file', 'body', 'file', true, NULL);
-- Optional: also declare a text field on the same route
-- INSERT INTO param_schema ... ('upload', 'description', 'body', 'string', false, NULL);
```

A curl invocation:
```
curl -F "file=@myfile.txt" -F "description=hello" http://localhost:18300/upload
```

---

## 8. Oracle SQL contract

The `is_multipart_ct` CTE detects multipart. `multipart_boundary` extracts the boundary.
`multipart_parts` splits on `\r\n--<boundary>`. `multipart_map` builds a MAP of
`name -> content` and a parallel `MAP of name__filename -> filename`. These feed
`param_values` alongside form_map (is_multipart_ct takes priority over is_form_ct).

The C mirror in `quackapi_brain.cpp` replicates the parse identically using
`strtok_r`-style boundary scanning and `quack_multipart_extract` / `quack_multipart_extract_filename`.

---

## 9. Deviations from FastAPI

1. **`type='file'` in param_schema instead of `UploadFile`**: No Python object model.
   File content is a VARCHAR (text-safe only — see §6).
2. **`multipart_parse` error type (422 not 400)**: Structural parse failures return
   422 (quackapi's single error path) rather than 400 (Starlette's parser exception).
3. **No per-part Content-Type routing**: FastAPI allows per-field `media_type` on
   `UploadFile`; quackapi passes the value through but does not gate on it.
4. **No spooling to disk**: FastAPI spools large uploads to a temp file. quackapi holds
   the entire body in a 64 KB C buffer (handle_conn_on single read) and in a VARCHAR
   in SQL. Large uploads are truncated at 64 KB on the C path (existing limitation,
   not introduced by multipart).
5. **Binary safety**: FastAPI handles binary transparently. quackapi v1 is text-safe only.
6. **filename__ companion param**: No FastAPI analog (FastAPI exposes `.filename` on
   the `UploadFile` object). quackapi surfaces it as `{<name>__filename}` in the
   handler template.
