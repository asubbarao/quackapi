# One row per operation the server documents: its path, method, response envelope and parameters.
.paths | to_entries[] | .key as $path | .value | to_entries[]
| {op: .value.operationId, path: $path, method: .key,
   envelope: .value.responses."200".content."application/json".schema.type,
   param_names: [(.value.parameters // [])[].name],
   params: [(.value.parameters // []) | to_entries[]
            | {position: .key, name: .value.name, place: .value.in, required: .value.required,
               type: .value.schema.type, format: .value.schema.format}]}
