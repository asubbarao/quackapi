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
// DuckDB 2.0 also provides a managed transport pool. This explicit-client bridge
// preserves QuackAPI's existing per-call reused_connection and per-host pool
// counters. Query scheduling and I/O task ownership use the native scheduler;
// no outbound worker threads are created here.
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
	//! stall_ms > 0 (plain http:// only): after sending the request, wait that
	//! many milliseconds before reading the response. Test seam for server
	//! write-timeout / per-route timeout_sec — a compute-bound handler cannot
	//! trip socket IO deadlines.
	static QuackapiHttpFetchResult Get(DatabaseInstance &db, const string &url,
	                                   const unordered_map<string, string> &extra_headers = {}, int32_t stall_ms = 0);

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

//! quackapi_fetch / quackapi_post / quackapi_http_pool / quackapi_parallel_fetch.
void RegisterQuackapiHttpFetchFunctions(ExtensionLoader &loader);

} // namespace duckdb
