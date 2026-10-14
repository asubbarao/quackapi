#include "quackapi_post_provider.hpp"

#include "quackapi_post_executor.hpp"
#include "quackapi_limits.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <typeinfo>

namespace duckdb {

namespace {

constexpr int64_t OUTBOUND_TIMEOUT_FALLBACK_MS = 600 * 1000;

//! Connection settings that must match before a worker may reuse its provider
//! client. Request headers and timeouts stay request-local.
struct ClientConfig {
	string provider_name;
	string params_type;
	string origin;
	string http_proxy;
	idx_t http_proxy_port = 0;
	string http_proxy_username;
	string http_proxy_password;
	bool keep_alive = true;
	bool follow_location = true;
	bool override_verify_ssl = false;
	bool verify_ssl = true;

	bool Matches(const HTTPUtil &util, const HTTPParams &params, const string &candidate_origin) const {
		// The common fields below cannot fingerprint provider-specific parameter
		// subclasses. Until a provider exposes a configuration comparison API,
		// only reuse the exact base parameter type; replace provider-specialized
		// clients for every request rather than risk carrying stale settings.
		return typeid(params) == typeid(HTTPParams) && provider_name == util.GetName() &&
		       params_type == typeid(params).name() && origin == candidate_origin && http_proxy == params.http_proxy &&
		       (http_proxy.empty() || http_proxy_port == params.http_proxy_port) &&
		       http_proxy_username == params.http_proxy_username && http_proxy_password == params.http_proxy_password &&
		       keep_alive == params.keep_alive && follow_location == params.follow_location &&
		       override_verify_ssl == params.override_verify_ssl && verify_ssl == params.verify_ssl;
	}

	void Set(const HTTPUtil &util, const HTTPParams &params, const string &candidate_origin) {
		provider_name = util.GetName();
		params_type = typeid(params).name();
		origin = candidate_origin;
		http_proxy = params.http_proxy;
		http_proxy_port = http_proxy.empty() ? 0 : params.http_proxy_port;
		http_proxy_username = params.http_proxy_username;
		http_proxy_password = params.http_proxy_password;
		keep_alive = params.keep_alive;
		follow_location = params.follow_location;
		override_verify_ssl = params.override_verify_ssl;
		verify_ssl = params.verify_ssl;
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
	const auto timeout_ms = QuackapiRemainingTimeoutMillis(OUTBOUND_TIMEOUT_FALLBACK_MS);
	params->timeout = static_cast<uint64_t>(timeout_ms / 1000);
	params->timeout_usec = static_cast<uint64_t>((timeout_ms % 1000) * 1000);

	const bool reused = state.client && state.client_util == &http_util && state.has_client_config &&
	                    state.client_config.Matches(http_util, *params, origin);
	if (!reused) {
		DiscardClient(state);
		state.client = http_util.InitializeClient(*params, origin);
		if (!state.client) {
			throw InvalidConfigurationException(UnsupportedProviderMessage(provider_name));
		}
		state.client_util = &http_util;
		state.client_config.Set(http_util, *params, origin);
		state.has_client_config = true;
	} else {
		// Timeouts are request-specific. Reapply them to a worker-reused client.
		state.client->Initialize(*params);
	}

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

	// The caller can be interrupted while this worker is being scheduled or
	// while its provider client is prepared. Never send after that signal.
	ThrowIfCancelled(request);
	QuackapiHttpFetchResult result;
	try {
		result = FromResponse(http_util.Request(info, state.client));
	} catch (NotImplementedException &) {
		DiscardClient(state);
		throw InvalidConfigurationException(UnsupportedProviderMessage(provider_name));
	}
	result.reused_connection = reused;
	if (!result.request_error.empty()) {
		DiscardClient(state);
	}
	return result;
}

} // namespace duckdb
