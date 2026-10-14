#pragma once

#include <utility>

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/types.hpp"
#include "httplib.hpp"

namespace duckdb {

//! Preserve whether a JSON member was an actual JSON null. A string value of
//! "null" remains a string and must not silently become SQL NULL.
struct JsonBodyField {
	string value;
	bool explicit_null;

	JsonBodyField() : explicit_null(false) {
	}
	JsonBodyField(string value_p, bool explicit_null_p) : value(std::move(value_p)), explicit_null(explicit_null_p) {
	}
};

bool IsJsonMediaType(const string &media);
bool IsFormUrlEncodedMediaType(const string &media);
bool IsMultipartMediaType(const string &media);
bool IsBodyMethod(const string &method);
void ParseFormUrlEncoded(const string &body, case_insensitive_map_t<string> &out);
bool TryExtractFlatJsonObject(const string &raw_body, case_insensitive_map_t<JsonBodyField> &fields);
void ParseQueryStringIntoParams(const string &query, duckdb_httplib::Params &params);

} // namespace duckdb
