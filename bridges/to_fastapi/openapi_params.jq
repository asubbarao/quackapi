# One row per parameter of each operation the server documents.
.paths[][] | .operationId as $op | (.parameters // []) | to_entries[]
| {op: $op, position: .key, name: .value.name, place: .value.in, required: .value.required,
   type: .value.schema.type, format: .value.schema.format}
