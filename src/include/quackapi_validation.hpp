#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {

//! One FastAPI-compatible request-validation issue. loc_json is deliberately a
//! JSON array so body paths may contain array indexes as numbers.
struct QuackapiValidationIssue {
	string loc_json;
	string message;
	string type;
};

//! Serialize one or many request-validation issues as FastAPI's 422 detail
//! envelope. The caller supplies a complete, JSON-safe location array.
string QuackapiValidationErrorsJson(const vector<QuackapiValidationIssue> &issues);

//! Convert a JSON Pointer (RFC 6901) reported by a validator into a FastAPI
//! body location, e.g. /items/0/sku -> ["body","items",0,"sku"]. json_body is
//! the validated request payload and distinguishes a numeric object key from
//! an array index.
string QuackapiValidationBodyPointerLoc(const string &pointer, const string &json_body);

//! Convert DuckDB's json_transform structure syntax into the compatible
//! OpenAPI schema fragment. The declaration remains the runtime source of
//! truth; this only projects its native STRUCT/LIST/scalar shape for clients.
string QuackapiDuckdbTransformToOpenApi(const string &body_type);

} // namespace duckdb
