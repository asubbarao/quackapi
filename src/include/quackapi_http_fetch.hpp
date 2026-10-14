//===----------------------------------------------------------------------===//
// quackapi_http_fetch.hpp
//
// Outbound HTTP behind quackapi_post. Two paths, one rule: the TCP connection
// MUST survive across requests.
//
//   1. http://   → the VENDORED httplib client, checked out of a per-host
//                  free-list pool. Keep-alive + TCP_NODELAY. Full method
//                  support, no companion extension required.
//   2. https://  → DuckDB's HTTPUtil, so `LOAD curl_httpfs` / `LOAD httpfs`
//                  transparently supplies TLS. Pooled the same way, via
//                  HTTPUtil::Request(request, client) which reuses the client
//                  we hand it instead of building a fresh one.
//
// Why the pool exists: HTTPUtil::Request(request) — the single-argument form —
// declares a local `unique_ptr<HTTPClient> client;` and lets it die at the end
// of the call (duckdb/src/main/http/http_util.cpp:128). Every request therefore
// pays a fresh DNS + TCP + (TLS) handshake. A route handler makes exactly one
// call per request: the worst case.
//
// GETs are not served here: use the community http_client extension
// (http_get / http_head) or read_text through curl_httpfs.
//
// Never shell out to the curl binary. Never link libcurl into quackapi.
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/http_util.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

class ClientContext;

//! Result of an in-database outbound HTTP call.
struct QuackapiHttpFetchResult {
	HTTPStatusCode status = HTTPStatusCode::INVALID;
	string body;
	string reason;
	bool success = false;
	//! Empty unless the transport failed before a response (DNS, connect, …).
	string request_error;
	HTTPHeaders headers;
	//! True when this call reused a pooled connection rather than dialling.
	bool reused_connection = false;

	bool Ok() const {
		return success && request_error.empty();
	}
};

//! Outbound limits, read from the SET quackapi_http_* settings at call time.
struct QuackapiHttpClientLimits {
	//! Max concurrent sends for one chunk of rows. 0 = no cap (every row in flight).
	idx_t fanout = 0;
	//! Idle keep-alive connections kept per host. 0 = keep every connection that
	//! was in flight, so the next burst of the same width dials nothing.
	idx_t max_idle_per_host = 0;
	//! TCP connect deadline in milliseconds.
	int64_t connect_timeout_ms = 5000;
	//! Read/write deadline in milliseconds. A request deadline, when tighter, wins.
	int64_t timeout_ms = 600000;

	static QuackapiHttpClientLimits FromContext(ClientContext &context);
};

struct QuackapiHttpFetch {
	//! POST url with a raw body and Content-Type.
	//! http:// needs nothing loaded; https:// requires httpfs / curl_httpfs.
	static QuackapiHttpFetchResult Post(DatabaseInstance &db, const string &url, const string &body,
	                                    const string &content_type, const unordered_map<string, string> &extra_headers,
	                                    const QuackapiHttpClientLimits &limits);

	//! Drop every idle connection (server stop; idle clients must not pin workers).
	static void ResetPool();
};

//! SET quackapi_http_* options read by quackapi_post.
void RegisterQuackapiHttpClientSettings(DBConfig &config);

//! quackapi_post.
void RegisterQuackapiHttpFetchFunctions(ExtensionLoader &loader);

} // namespace duckdb
