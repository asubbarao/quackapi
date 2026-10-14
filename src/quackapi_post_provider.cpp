#include "quackapi_post_provider.hpp"

#include "quackapi_post_executor.hpp"
#include "quackapi_limits.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

namespace {

constexpr int64_t OUTBOUND_TIMEOUT_FALLBACK_MS = 600 * 1000;

//! Identity of a provider client that may be reinitialized by the next request.
struct ClientConfig {
	string provider_name;
	string origin;

	bool Matches(const HTTPUtil &util, const string &candidate_origin) const {
		const auto name = util.GetName();
		const bool reviewed_provider = name == "HTTPFS-Curl" || name == "MultiCurl";
		return reviewed_provider && provider_name == name && origin == candidate_origin;
	}

	void Set(const HTTPUtil &util, const string &candidate_origin) {
		provider_name = util.GetName();
		origin = candidate_origin;
	}
};

struct WorkerState : public QuackapiPostWorkerState {
	unique_ptr<HTTPClient> client;
	HTTPUtil *client_util = nullptr;
	ClientConfig client_config;
	bool has_client_config = false;

	void Reset() {
		client.reset();
		client_util = nullptr;
		has_client_config = false;
	}
};

void InsertExtraHeaders(HTTPHeaders &headers, const unordered_map<string, string> &extra_headers) {
	for (auto &header : extra_headers) {
		headers.Insert(header.first, header.second);
	}
}

QuackapiHttpFetchResult FromResponse(unique_ptr<HTTPResponse> response) {
	QuackapiHttpFetchResult result;
	if (!response) {
		result.request_error = "null HTTP response";
		return result;
	}
	result.status = response->status;
	result.body = std::move(response->body);
	result.reason = std::move(response->reason);
	result.success = response->Success();
	result.request_error = response->GetRequestError();
	result.headers = std::move(response->headers);
	return result;
}

bool IsCancelled(const QuackapiPostRequest &request) {
	if (request.cancelled && request.cancelled->load()) {
		return true;
	}
	return std::chrono::steady_clock::now() >= QuackapiCurrentDeadline();
}

void ThrowIfCancelled(const QuackapiPostRequest &request) {
	if (IsCancelled(request)) {
		throw InterruptException();
	}
}

string UnsupportedProviderMessage(const string &provider_name) {
	return StringUtil::Format("quackapi_post requires an explicitly loaded HTTPUtil provider that supports POST. "
	                          "Load curl_httpfs (recommended) or httpfs, then retry. Active HTTPUtil is '%s'.",
	                          provider_name);
}

void DiscardClient(WorkerState &state) {
	state.Reset();
}

} // namespace

unique_ptr<QuackapiPostWorkerState> QuackapiPostProvider::CreateWorkerState(DatabaseInstance &) {
	return make_uniq<WorkerState>();
}

QuackapiHttpFetchResult QuackapiPostProvider::Post(DatabaseInstance &db, QuackapiPostWorkerState &state_p,
                                                   const QuackapiPostRequest &request) {
	ThrowIfCancelled(request);
	auto &state = static_cast<WorkerState &>(state_p);
	auto &http_util = HTTPUtil::Get(db);
	const auto provider_name = http_util.GetName();
	if (provider_name == "Built-In") {
		throw InvalidConfigurationException(UnsupportedProviderMessage(provider_name));
	}

	string path;
	string origin;
	HTTPUtil::DecomposeURL(request.url, path, origin);
	auto params = http_util.InitializeParameters(db, request.url);
	params->keep_alive = true;
	params->retries = 0; // POST is not safe to replay after an ambiguous transport failure.
	const bool reused = state.client && state.client_util == &http_util && state.has_client_config &&
	                    state.client_config.Matches(http_util, origin);

	HTTPHeaders headers(db);
	if (!request.content_type.empty()) {
		headers.Insert("Content-Type", request.content_type);
	} else if (StringUtil::StartsWith(StringUtil::Lower(request.url), "http://")) {
		// Preserve the previous plain-HTTP empty-content-type default.
		headers.Insert("Content-Type", "application/octet-stream");
	}
	InsertExtraHeaders(headers, request.headers);
	PostRequestInfo info(request.url, headers, *params, const_data_ptr_cast(request.body.data()), request.body.size());
	info.try_request = true;

	// Prepare the provider client with the latest remaining deadline. Parameter
	// initialization and client setup happen before this point and can consume
	// part of the caller's budget.
	ThrowIfCancelled(request);
	const auto timeout_ms = QuackapiRemainingTimeoutMillis(OUTBOUND_TIMEOUT_FALLBACK_MS);
	params->timeout = static_cast<uint64_t>(timeout_ms / 1000);
	params->timeout_usec = static_cast<uint64_t>((timeout_ms % 1000) * 1000);
	if (!reused) {
		DiscardClient(state);
		state.client = http_util.InitializeClient(*params, origin);
		if (!state.client) {
			throw InvalidConfigurationException(UnsupportedProviderMessage(provider_name));
		}
		state.client_util = &http_util;
		state.client_config.Set(http_util, origin);
		state.has_client_config = true;
	}

	// Client setup itself may use part of the remaining budget. Refresh timeout
	// parameters and initialize with the current configuration before sending.
	const auto send_timeout_ms = QuackapiRemainingTimeoutMillis(OUTBOUND_TIMEOUT_FALLBACK_MS);
	params->timeout = static_cast<uint64_t>(send_timeout_ms / 1000);
	params->timeout_usec = static_cast<uint64_t>((send_timeout_ms % 1000) * 1000);
	// The reviewed providers fully apply current HTTPParams on Initialize. This
	// includes changed proxy/TLS/auth settings; no short-lived params reference
	// is retained. For fresh clients this also replaces the initial timeout.
	state.client->Initialize(*params);

	// Never send after setup consumes the remaining deadline.
	ThrowIfCancelled(request);
	QuackapiHttpFetchResult result;
	try {
		result = FromResponse(http_util.Request(info, state.client));
	} catch (NotImplementedException &) {
		DiscardClient(state);
		throw InvalidConfigurationException(UnsupportedProviderMessage(provider_name));
	}
	// This reports HTTPClient object reuse only; the provider does not expose
	// whether its underlying TCP socket was reused.
	result.reused_connection = reused;
	if (!result.request_error.empty()) {
		DiscardClient(state);
	}
	return result;
}

} // namespace duckdb
