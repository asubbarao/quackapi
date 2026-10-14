//===----------------------------------------------------------------------===//
// quackapi_post_provider.hpp
//
// Provider boundary for outbound POST. The implementation currently preserves
// the existing HTTP/HTTPS transports; later provider work can replace it
// without changing the SQL surface or scheduler.
//===----------------------------------------------------------------------===//
#pragma once

#include "quackapi_http_fetch.hpp"

namespace duckdb {

struct QuackapiPostProvider {
	static QuackapiHttpFetchResult Post(DatabaseInstance &db, const string &url, const string &body,
	                                    const string &content_type, const unordered_map<string, string> &extra_headers);
};

} // namespace duckdb
