//===----------------------------------------------------------------------===//
// quackapi_http_fetch.hpp
//
// Outbound HTTP for quackapi. Two paths, one rule: the TCP connection MUST
// survive across requests.
//
//   1. http://   → the VENDORED httplib client, checked out of a per-host
//                  free-list pool (QuackapiHttpPool). Keep-alive + TCP_NODELAY.
//                  Full method support, no companion extension required.
//   2. https://  → DuckDB's HTTPUtil, so `LOAD curl_httpfs` / `LOAD httpfs`
//                  transparently supplies TLS. Pooled the same way, via
//                  HTTPUtil::Request(request, client) which reuses the client
//                  we hand it instead of building a fresh one.
//
// Why the pool exists: HTTPUtil::Request(request) — the single-argument form —
// declares a local `unique_ptr<HTTPClient> client;` and lets it die at the end
// of the call (duckdb/src/main/http/http_util.cpp:128). Every request therefore
// pays a fresh DNS + TCP + (TLS) handshake. The community `http_client`
// extension has the same shape, which is why a route calling http_post() once
// per request measured ~40ms against an upstream whose own floor was 13ms,
// flat across worker_threads — while ten calls inside ONE query amortised to
// 9.2ms. Connections were only ever reused WITHIN a query execution. A route
// handler makes exactly one call per request: the worst case.
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

//! Snapshot of the outbound connection pool, exposed as quackapi_http_pool().
struct QuackapiHttpPoolStats {
	string host;
	idx_t idle = 0;
	idx_t dialed = 0;
	idx_t reused = 0;
};

struct QuackapiHttpFetch {
	//! Active util name: "MultiCurl", "HTTPFS-Curl", "HTTPFS", "Built-In", …
	static string ActiveHttpUtilName(DatabaseInstance &db);

	//! GET url. headers are optional extra request headers (e.g. Authorization).
	static QuackapiHttpFetchResult Get(DatabaseInstance &db, const string &url,
	                                   const unordered_map<string, string> &extra_headers = {});

	//! POST url with a raw body and Content-Type.
	//! http:// needs nothing loaded; https:// requires httpfs / curl_httpfs.
	static QuackapiHttpFetchResult Post(DatabaseInstance &db, const string &url, const string &body,
	                                    const string &content_type = "application/x-www-form-urlencoded",
	                                    const unordered_map<string, string> &extra_headers = {});

	//! Per-host pool counters, for tests and quackapi_http_pool().
	static vector<QuackapiHttpPoolStats> PoolStats();
	//! Drop every idle connection (test isolation; not needed in production).
	static void ResetPool();
};

//! quackapi_fetch / quackapi_post / quackapi_http_pool.
void RegisterQuackapiHttpFetchFunctions(ExtensionLoader &loader);

} // namespace duckdb
