# Native request validation and Pydantic parity

QuackAPI validates request inputs with DuckDB values and JSON rather than a
mandatory Python/Pydantic process. It keeps FastAPI-compatible 422 envelopes
(detail[].loc, msg, type) so existing clients can handle validation responses
without a second error protocol.

## Use DuckDB types for a request body

~~~
CREATE ROUTE create_cart POST '/carts'
  BODY TYPE '{"items":[{"sku":"VARCHAR","qty":"INTEGER"}]}'
  AS
SELECT array_length($body.items) AS item_count;
~~~

BODY TYPE is the native DuckDB json_transform structure syntax. At request time
QuackAPI calls json_transform_strict, derives the matching DuckDB type, and
binds $body as the corresponding STRUCT, LIST, or scalar. The route handler
uses $body.items directly; it does not repeat a STRUCT cast. No Pydantic
runtime and no JSON Schema extension are needed for this path. A failed
transform returns 422 before handler SQL runs.

BODY TYPE accepts values DuckDB can convert, including compatible JSON strings
for numeric columns. For exact JSON-token types and field constraints, compose
it with BODY SCHEMA:

~~~
CREATE ROUTE create_cart_strict POST '/carts/strict'
  BODY TYPE '{"items":[{"sku":"VARCHAR","qty":"INTEGER"}]}'
  BODY SCHEMA '{"type":"object","required":["items"],"properties":{"items":{"type":"array","items":{"type":"object","required":["sku","qty"],"properties":{"sku":{"type":"string","minLength":1},"qty":{"type":"integer","minimum":1}}}}}}'
  AS SELECT array_length($body.items) AS item_count;
~~~

BODY SCHEMA is the strict/constraint layer: it supplies JSON Schema type,
required, bounds, length, enum, pattern, and nested-array checks. The optional
community json_schema extension is used only when that clause is declared.

## Error and null behavior

| Input situation | Native behavior |
|---|---|
| Several missing or invalid handler parameters | One 422 containing every independent field error; locations retain body, path, query, header, or cookie. |
| Nested BODY SCHEMA failure | A validator JSON Pointer such as /items/0/qty becomes ["body","items",0,"qty"]. |
| Field absent | PARAM ... DEFAULT ... applies its declared default; DEFAULT NULL binds SQL NULL. |
| Explicit JSON null | Distinct from a missing member and from the string "null". It binds SQL NULL only when a BODY SCHEMA accepts null or the parameter declares DEFAULT NULL. |
| JSON string "null" | Remains a string and is type-checked normally. |
| Query/path integers | Reject decimal and scientific spellings that DuckDB would otherwise round; constraints run after a successful native cast. |

## OpenAPI

/openapi.json publishes BODY SCHEMA directly as the operation requestBody. A
BODY TYPE-only route recursively projects its declared STRUCT fields, LIST
items, and scalar formats into OpenAPI properties, items, and types. The exact
native declaration remains in x-duckdb-json-transform. When both clauses are
present, OpenAPI intersects the JSON Schema and the generated native shape.

## Deliberate boundaries

The native surface covers typed scalar, root LIST, and STRUCT request bodies;
required/default/null behavior; aggregate parameter errors; body-path errors;
and numeric/string parameter constraints. It does not import or execute Python
field_validator/model_validator functions, Python default_factory, or arbitrary
custom Pydantic types. Express those policies as DuckDB SQL, constraints, or a
declared JSON Schema and keep the validation close to the tables that own the
data.

FastAPI documents nested models and lists as request-body data that is
converted, validated, and included in OpenAPI. Pydantic documents coercion as
the default and strict validation as an opt-in configuration. QuackAPI maps
those contracts onto DuckDB typed values and explicit schema clauses:
[FastAPI nested bodies](https://fastapi.tiangolo.com/tutorial/body-nested-models/),
[FastAPI request bodies](https://fastapi.tiangolo.com/tutorial/body/), and
[Pydantic strict configuration](https://docs.pydantic.dev/latest/api/config/).
