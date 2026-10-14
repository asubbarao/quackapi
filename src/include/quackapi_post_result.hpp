//===----------------------------------------------------------------------===//
// quackapi_post_result.hpp
//
// Shared response type and SQL conversion helpers for quackapi_post.
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/http_util.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {

//! Result of an outbound POST call.
struct QuackapiHttpFetchResult {
	HTTPStatusCode status = HTTPStatusCode::INVALID;
	string body;
	string reason;
	bool success = false;
	//! Empty unless the transport failed before a response (DNS, connect, …).
	string request_error;
	HTTPHeaders headers;
	//! True when this call reused a provider connection.
	bool reused_connection = false;

	bool Ok() const {
		return success && request_error.empty();
	}
};

LogicalType QuackapiHttpFetchResultType();
Value QuackapiHttpFetchResultValue(const QuackapiHttpFetchResult &result);
unordered_map<string, string> QuackapiHttpHeadersFromValue(const Value &value);

} // namespace duckdb
