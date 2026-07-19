#include "quackapi_http_fetch.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

namespace {

void InsertExtraHeaders(HTTPHeaders &headers, const unordered_map<string, string> &extra) {
	for (auto &kv : extra) {
		headers.Insert(kv.first, kv.second);
	}
}

QuackapiHttpFetchResult FromResponse(unique_ptr<HTTPResponse> response) {
	QuackapiHttpFetchResult out;
	if (!response) {
		out.request_error = "null HTTP response";
		return out;
	}
	out.status = response->status;
	out.body = std::move(response->body);
	out.reason = std::move(response->reason);
	out.success = response->Success();
	out.request_error = response->GetRequestError();
	out.headers = std::move(response->headers);
	return out;
}

} // namespace

string QuackapiHttpFetch::ActiveHttpUtilName(DatabaseInstance &db) {
	return HTTPUtil::Get(db).GetName();
}

QuackapiHttpFetchResult QuackapiHttpFetch::Get(DatabaseInstance &db, const string &url,
                                               const unordered_map<string, string> &extra_headers) {
	auto &http_util = HTTPUtil::Get(db);
	auto params = http_util.InitializeParameters(db, url);
	HTTPHeaders headers(db);
	InsertExtraHeaders(headers, extra_headers);

	GetRequestInfo request(url, headers, *params, /*response_handler=*/nullptr, /*content_handler=*/nullptr);
	request.try_request = true;

	return FromResponse(http_util.Request(request));
}

QuackapiHttpFetchResult QuackapiHttpFetch::Post(DatabaseInstance &db, const string &url, const string &body,
                                                const string &content_type,
                                                const unordered_map<string, string> &extra_headers) {
	// Built-In HTTPLibClient does not implement POST (http_util.cpp). Surface a
	// clear error pointing operators at curl_httpfs / httpfs rather than a raw
	// NotImplementedException deep in the client.
	auto &http_util = HTTPUtil::Get(db);
	const auto util_name = http_util.GetName();
	if (util_name == "Built-In") {
		throw InvalidConfigurationException(
		    "quackapi outbound POST requires an HTTP client with full method support. "
		    "LOAD curl_httpfs (recommended) or LOAD httpfs, then retry. Active HTTPUtil is '%s'.",
		    util_name);
	}

	auto params = http_util.InitializeParameters(db, url);
	HTTPHeaders headers(db);
	if (!content_type.empty()) {
		headers.Insert("Content-Type", content_type);
	}
	InsertExtraHeaders(headers, extra_headers);

	PostRequestInfo request(url, headers, *params, const_data_ptr_cast(body.data()), body.size());
	request.try_request = true;

	return FromResponse(http_util.Request(request));
}

} // namespace duckdb
