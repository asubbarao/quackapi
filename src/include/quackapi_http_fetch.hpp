//===----------------------------------------------------------------------===//
// quackapi_http_fetch.hpp
//
// Outbound HTTP for quackapi — ALWAYS goes through DuckDB's HTTPUtil so that
// loading curl_httpfs (or httpfs) transparently upgrades the client:
//
//   LOAD curl_httpfs;  -- config.SetHTTPUtil(MultiCurlUtil)
//   // subsequent QuackapiHttpFetch::* calls use MultiCurl / HTTPFS-Curl
//
// Never shell out to the curl binary. Never link libcurl into quackapi.
// See /tmp/quackapi_curlhttpfs/FINDINGS.md for the evidence trail.
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/http_util.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

//! Result of an in-database outbound HTTP call made via HTTPUtil.
struct QuackapiHttpFetchResult {
	HTTPStatusCode status = HTTPStatusCode::INVALID;
	string body;
	string reason;
	bool success = false;
	//! Empty unless the transport failed before a response (DNS, connect, …).
	string request_error;
	HTTPHeaders headers;

	bool Ok() const {
		return success && request_error.empty();
	}
};

//! Thin wrappers around HTTPUtil::Get(db).Request(...).
//! Used by the planned OAuth/OIDC auth wave (token exchange, discovery).
//! Route handlers that only need GET of text/bytes should prefer SQL
//! `read_text` / `read_blob` so they benefit from httpfs caching & secrets.
struct QuackapiHttpFetch {
	//! Active util name: "MultiCurl", "HTTPFS-Curl", "HTTPFS", "Built-In", …
	static string ActiveHttpUtilName(DatabaseInstance &db);

	//! GET url. headers are optional extra request headers (e.g. Authorization).
	static QuackapiHttpFetchResult Get(DatabaseInstance &db, const string &url,
	                                   const unordered_map<string, string> &extra_headers = {});

	//! POST url with a raw body and Content-Type (OAuth token endpoint, etc.).
	//! Requires an HTTPUtil that implements POST (httpfs / curl_httpfs). The
	//! built-in util's client throws NotImplementedException on POST.
	static QuackapiHttpFetchResult Post(DatabaseInstance &db, const string &url, const string &body,
	                                    const string &content_type = "application/x-www-form-urlencoded",
	                                    const unordered_map<string, string> &extra_headers = {});
};

} // namespace duckdb
