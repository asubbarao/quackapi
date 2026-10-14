#include "quackapi_http_fetch.hpp"
#include "quackapi_thread_join.hpp"
#include "quackapi_limits.hpp"

#include <atomic>
#include <mutex>
#include <thread>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/client_context.hpp"

#include "httplib.hpp"

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

//! A positive cap that the caller set, or 0 when it means "no cap".
bool UnderIdleCap(idx_t idle, const QuackapiHttpClientLimits &limits) {
	return limits.max_idle_per_host == 0 || idle < limits.max_idle_per_host;
}

//===--------------------------------------------------------------------===//
// Connection pool
//===--------------------------------------------------------------------===//
// Free-list per proto_host_port. Acquire pops (or dials); Release pushes back.
// A pool rather than one shared client per host on purpose: httplib::Client
// serialises requests on its socket, so a single shared client would trade the
// handshake cost for a concurrency ceiling of 1. N idle clients give both reuse
// and parallelism — the same shape as httpx's pool on the FastAPI side.

struct PooledPlain {
	unique_ptr<duckdb_httplib::Client> client;
};

struct PooledUtil {
	unique_ptr<HTTPClient> client;
};

struct HostPool {
	vector<PooledPlain> plain_idle;
	vector<PooledUtil> util_idle;
};

struct ConnectionPool {
	static ConnectionPool &Get() {
		static ConnectionPool instance;
		return instance;
	}

	std::mutex lock;
	unordered_map<string, HostPool> hosts;
};

//! Split "http://host:port/a/b?c" into origin + path. Mirrors HTTPUtil::DecomposeURL.
void SplitURL(const string &url, string &origin, string &path) {
	HTTPUtil::DecomposeURL(url, path, origin);
	if (path.empty()) {
		path = "/";
	}
}

bool IsPlainHTTP(const string &url) {
	return StringUtil::StartsWith(StringUtil::Lower(url), "http://");
}

void SetTimeouts(duckdb_httplib::Client &client, int64_t timeout_ms, int64_t connect_ms) {
	client.set_max_timeout(static_cast<time_t>(timeout_ms));
	client.set_read_timeout(static_cast<time_t>(timeout_ms / 1000), static_cast<time_t>((timeout_ms % 1000) * 1000));
	client.set_write_timeout(static_cast<time_t>(timeout_ms / 1000), static_cast<time_t>((timeout_ms % 1000) * 1000));
	client.set_connection_timeout(static_cast<time_t>(connect_ms / 1000),
	                              static_cast<time_t>((connect_ms % 1000) * 1000));
}

unique_ptr<duckdb_httplib::Client> DialPlain(const string &origin) {
	auto client = make_uniq<duckdb_httplib::Client>(origin);
	client->set_keep_alive(true);
	// Same Nagle/delayed-ACK deadlock the server side hit (quackapi_server.cpp:917),
	// just on the other end of the socket: without this a small POST body waits on
	// the peer's delayed ACK before the request is even complete.
	client->set_tcp_nodelay(true);
	client->set_follow_location(true);
	client->set_decompress(true);
	return client;
}

duckdb_httplib::Headers ToHttplibHeaders(const unordered_map<string, string> &extra) {
	duckdb_httplib::Headers headers;
	for (auto &kv : extra) {
		headers.emplace(kv.first, kv.second);
	}
	return headers;
}

QuackapiHttpFetchResult FromHttplibResult(const duckdb_httplib::Result &res) {
	QuackapiHttpFetchResult out;
	if (res.error() != duckdb_httplib::Error::Success) {
		out.request_error = duckdb_httplib::to_string(res.error());
		return out;
	}
	auto &response = res.value();
	out.status = HTTPUtil::ToStatusCode(response.status);
	out.body = response.body;
	out.reason = response.reason;
	out.success = response.status >= 200 && response.status < 400;
	for (auto &entry : response.headers) {
		out.headers.Insert(entry.first, entry.second);
	}
	return out;
}

//! Run `call` against a pooled plain-HTTP client for `origin`, then return the
//! client to the pool. The client is only recycled when the transport stayed
//! healthy — a broken socket must not be handed to the next request.
template <class CALL>
QuackapiHttpFetchResult WithPlainClient(const string &origin, const QuackapiHttpClientLimits &limits, CALL &&call) {
	unique_ptr<duckdb_httplib::Client> client;
	bool reused = false;
	{
		auto &pool = ConnectionPool::Get();
		std::lock_guard<std::mutex> guard(pool.lock);
		auto &host = pool.hosts[origin];
		if (!host.plain_idle.empty()) {
			client = std::move(host.plain_idle.back().client);
			host.plain_idle.pop_back();
			reused = true;
		}
	}
	if (!client) {
		client = DialPlain(origin);
	}
	// Reset on every checkout: a previous request's short deadline must not leak.
	const auto timeout_ms = QuackapiRemainingTimeoutMillis(limits.timeout_ms);
	SetTimeouts(*client, timeout_ms, std::min<int64_t>(timeout_ms, limits.connect_timeout_ms));

	QuackapiHttpFetchResult result;
	bool healthy = false;
	try {
		auto res = call(*client);
		healthy = res.error() == duckdb_httplib::Error::Success;
		result = FromHttplibResult(res);
	} catch (...) {
		// Drop the client on the floor; a half-written socket is not reusable.
		throw;
	}
	result.reused_connection = reused;

	if (healthy) {
		auto &pool = ConnectionPool::Get();
		std::lock_guard<std::mutex> guard(pool.lock);
		auto &host = pool.hosts[origin];
		if (UnderIdleCap(host.plain_idle.size(), limits)) {
			host.plain_idle.push_back(PooledPlain {std::move(client)});
		}
	}
	return result;
}

//! Same checkout/return dance for the HTTPUtil path (https, or a loaded
//! curl_httpfs the operator wants used). The two-argument HTTPUtil::Request
//! reuses the client we pass instead of constructing a throwaway one.
template <class BUILD>
QuackapiHttpFetchResult WithUtilClient(DatabaseInstance &db, const string &url, const QuackapiHttpClientLimits &limits,
                                       BUILD &&build) {
	auto &http_util = HTTPUtil::Get(db);
	string origin, path;
	SplitURL(url, origin, path);
	const auto key = http_util.GetName() + "|" + origin;

	unique_ptr<HTTPClient> client;
	bool reused = false;
	{
		auto &pool = ConnectionPool::Get();
		std::lock_guard<std::mutex> guard(pool.lock);
		auto &host = pool.hosts[key];
		if (!host.util_idle.empty()) {
			client = std::move(host.util_idle.back().client);
			host.util_idle.pop_back();
			reused = true;
		}
	}

	auto params = http_util.InitializeParameters(db, url);
	params->keep_alive = true;
	const auto timeout_ms = QuackapiRemainingTimeoutMillis(limits.timeout_ms);
	params->timeout = timeout_ms / 1000;
	params->timeout_usec = (timeout_ms % 1000) * 1000;
	auto result = build(http_util, *params, client);
	result.reused_connection = reused;

	if (client && result.request_error.empty()) {
		auto &pool = ConnectionPool::Get();
		std::lock_guard<std::mutex> guard(pool.lock);
		auto &host = pool.hosts[key];
		if (UnderIdleCap(host.util_idle.size(), limits)) {
			host.util_idle.push_back(PooledUtil {std::move(client)});
		}
	}
	return result;
}

int64_t ReadSetting(ClientContext &context, const string &name, int64_t fallback, int64_t minimum) {
	Value setting;
	int64_t value = fallback;
	if (context.TryGetCurrentSetting(name, setting) && !setting.IsNull()) {
		value = setting.GetValue<int64_t>();
	}
	if (value < minimum) {
		throw InvalidInputException("%s must be >= %lld", name, (long long)minimum);
	}
	return value;
}

} // namespace

//===--------------------------------------------------------------------===//
// Public API
//===--------------------------------------------------------------------===//

QuackapiHttpClientLimits QuackapiHttpClientLimits::FromContext(ClientContext &context) {
	QuackapiHttpClientLimits limits;
	limits.fanout = static_cast<idx_t>(ReadSetting(context, "quackapi_http_fanout", 0, 0));
	limits.max_idle_per_host = static_cast<idx_t>(ReadSetting(context, "quackapi_http_max_idle_per_host", 0, 0));
	limits.connect_timeout_ms = ReadSetting(context, "quackapi_http_connect_timeout_ms", 5000, 1);
	limits.timeout_ms = ReadSetting(context, "quackapi_http_timeout_ms", 600000, 1);
	return limits;
}

QuackapiHttpFetchResult QuackapiHttpFetch::Post(DatabaseInstance &db, const string &url, const string &body,
                                                const string &content_type,
                                                const unordered_map<string, string> &extra_headers,
                                                const QuackapiHttpClientLimits &limits) {
	if (IsPlainHTTP(url)) {
		// The vendored httplib implements POST, so plain HTTP needs no companion
		// extension at all — unlike the HTTPUtil path below.
		string origin, path;
		SplitURL(url, origin, path);
		auto headers = ToHttplibHeaders(extra_headers);
		const auto &ct = content_type.empty() ? string("application/octet-stream") : content_type;
		return WithPlainClient(origin, limits,
		                       [&](duckdb_httplib::Client &client) { return client.Post(path, headers, body, ct); });
	}

	// Built-In HTTPLibClient does not implement POST (http_util.cpp). Surface a
	// clear error pointing operators at curl_httpfs / httpfs rather than a raw
	// NotImplementedException deep in the client.
	auto &util = HTTPUtil::Get(db);
	const auto util_name = util.GetName();
	if (util_name == "Built-In") {
		throw InvalidConfigurationException(
		    "quackapi outbound POST over https requires an HTTP client with full method support. "
		    "LOAD curl_httpfs (recommended) or LOAD httpfs, then retry. Active HTTPUtil is '%s'.",
		    util_name);
	}

	return WithUtilClient(
	    db, url, limits, [&](HTTPUtil &http_util, HTTPParams &params, unique_ptr<HTTPClient> &client) {
		    HTTPHeaders headers(db);
		    if (!content_type.empty()) {
			    headers.Insert("Content-Type", content_type);
		    }
		    InsertExtraHeaders(headers, extra_headers);
		    PostRequestInfo request(url, headers, params, const_data_ptr_cast(body.data()), body.size());
		    request.try_request = true;
		    return FromResponse(http_util.Request(request, client));
	    });
}

void QuackapiHttpFetch::ResetPool() {
	// Drop idle keep-alive sockets *before* httplib Server join. Stop paths
	// detach ~QuackapiHttpServer; if pooled clients still hold the peer open,
	// worker join can block until read timeout (600s) and CI process exit hangs
	// with no log upload (Windows runner "lost communication").
	auto &pool = ConnectionPool::Get();
	std::lock_guard<std::mutex> guard(pool.lock);
	for (auto &entry : pool.hosts) {
		for (auto &plain : entry.second.plain_idle) {
			if (plain.client) {
				plain.client->stop();
			}
		}
		// HTTPUtil clients: unique_ptr reset is enough (no keep-alive join path
		// into our inbound Server).
		entry.second.plain_idle.clear();
		entry.second.util_idle.clear();
	}
	pool.hosts.clear();
}

void RegisterQuackapiHttpClientSettings(DBConfig &config) {
	config.AddExtensionOption("quackapi_http_fanout",
	                          "Maximum concurrent quackapi_post sends for one chunk of rows. Default 0: no cap, every "
	                          "row of the chunk is in flight at once. A positive value caps it.",
	                          LogicalType::BIGINT, Value::BIGINT(0));
	config.AddExtensionOption("quackapi_http_max_idle_per_host",
	                          "Idle keep-alive connections quackapi_post keeps per host. Default 0: keep as many as "
	                          "were in flight. A positive value caps it.",
	                          LogicalType::BIGINT, Value::BIGINT(0));
	config.AddExtensionOption("quackapi_http_connect_timeout_ms",
	                          "TCP connect timeout for quackapi_post, in milliseconds. Default 5000.",
	                          LogicalType::BIGINT, Value::BIGINT(5000));
	config.AddExtensionOption("quackapi_http_timeout_ms",
	                          "Read/write timeout for quackapi_post, in milliseconds. Default 600000 (10 minutes). "
	                          "A tighter route request deadline wins.",
	                          LogicalType::BIGINT, Value::BIGINT(600000));
}

//===--------------------------------------------------------------------===//
// SQL surface
//===--------------------------------------------------------------------===//

namespace {

LogicalType FetchResultType() {
	child_list_t<LogicalType> children;
	children.emplace_back("status", LogicalType::INTEGER);
	children.emplace_back("reason", LogicalType::VARCHAR);
	children.emplace_back("body", LogicalType::VARCHAR);
	children.emplace_back("headers", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
	children.emplace_back("error", LogicalType::VARCHAR);
	children.emplace_back("reused_connection", LogicalType::BOOLEAN);
	return LogicalType::STRUCT(std::move(children));
}

Value ToValue(const QuackapiHttpFetchResult &result) {
	vector<Value> header_keys;
	vector<Value> header_values;
	for (auto &entry : result.headers) {
		header_keys.emplace_back(entry.first);
		header_values.emplace_back(entry.second);
	}
	child_list_t<Value> children;
	children.emplace_back("status", Value::INTEGER(static_cast<int32_t>(result.status)));
	children.emplace_back("reason", Value(result.reason));
	children.emplace_back("body", Value(result.body));
	children.emplace_back("headers", Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(header_keys),
	                                            std::move(header_values)));
	children.emplace_back("error",
	                      result.request_error.empty() ? Value(LogicalType::VARCHAR) : Value(result.request_error));
	children.emplace_back("reused_connection", Value::BOOLEAN(result.reused_connection));
	return Value::STRUCT(std::move(children));
}

//! MAP(VARCHAR, VARCHAR) argument → header map. NULL / missing = no extras.
unordered_map<string, string> HeadersFromValue(const Value &value) {
	unordered_map<string, string> out;
	if (value.IsNull()) {
		return out;
	}
	auto &entries = MapValue::GetChildren(value);
	for (auto &entry : entries) {
		auto &kv = StructValue::GetChildren(entry);
		if (kv.size() != 2 || kv[0].IsNull()) {
			continue;
		}
		out[kv[0].ToString()] = kv[1].IsNull() ? string() : kv[1].ToString();
	}
	return out;
}

//! Issue one independent outbound call per row of a chunk, CONCURRENTLY.
//!
//! Why this exists: quackapi_post is a scalar function, so a chunk can carry up
//! to STANDARD_VECTOR_SIZE independent requests. Run serially, N calls against an
//! upstream with latency L cost N*L while the machine idles and the connection
//! pool sits unused. The pool already tolerates concurrent checkout (every path
//! takes ConnectionPool::lock), so fanning out is the whole fix. `fanout` caps
//! the number of sends in flight; 0 sends every row at once.
//!
//! `perform(i)` must be safe to call from any thread. Exceptions are captured per
//! row rather than allowed to escape: WithPlainClient deliberately rethrows on a
//! half-written socket, and an exception crossing a std::thread boundary would
//! call std::terminate and take the server down with it. Argument validation that
//! must still raise to the caller therefore happens BEFORE the fan-out.
//!
//! The single-request case — what a route handler doing one upstream call per
//! request looks like — stays fully inline. No thread is created for it.
template <class PERFORM>
void FanOutRequests(idx_t count, const vector<bool> &skip, idx_t fanout, vector<QuackapiHttpFetchResult> &out,
                    PERFORM &&perform) {
	const auto deadline = QuackapiCurrentDeadline();
	auto run_one = [&](idx_t i) {
		QuackapiSetCurrentDeadline(deadline);
		try {
			out[i] = perform(i);
		} catch (std::exception &e) {
			out[i].request_error = e.what();
		} catch (...) {
			out[i].request_error = "unknown error during outbound HTTP request";
		}
	};

	idx_t active = 0;
	for (idx_t i = 0; i < count; i++) {
		if (!skip[i]) {
			active++;
		}
	}
	if (active == 0) {
		return;
	}
	if (active == 1) {
		for (idx_t i = 0; i < count; i++) {
			if (!skip[i]) {
				run_one(i);
			}
		}
		return;
	}

	const idx_t n_threads = (fanout == 0 || active < fanout) ? active : fanout;
	std::atomic<idx_t> cursor {0};
	vector<std::thread> threads;
	threads.reserve(n_threads);
	JoinOutboundThreads<vector<std::thread>> join_threads {threads};
	for (idx_t t = 0; t < n_threads; t++) {
		threads.emplace_back([&]() {
			for (;;) {
				const idx_t i = cursor.fetch_add(1);
				if (i >= count) {
					return;
				}
				if (!skip[i]) {
					run_one(i);
				}
			}
		});
	}
}

void PostScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto &db = *context.db;
	const auto count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	// Settings are read here, on the calling thread, so an invalid value raises to
	// the caller instead of being captured into a per-row error by the fan-out.
	const auto limits = QuackapiHttpClientLimits::FromContext(context);

	vector<bool> skip(count, false);
	vector<string> urls(count);
	vector<string> bodies(count);
	vector<string> content_types(count);
	vector<unordered_map<string, string>> headers(count);
	for (idx_t i = 0; i < count; i++) {
		auto url = args.data[0].GetValue(i);
		if (url.IsNull()) {
			skip[i] = true;
			continue;
		}
		urls[i] = url.ToString();
		auto body = args.data[1].GetValue(i);
		bodies[i] = body.IsNull() ? string() : body.ToString();
		content_types[i] = "application/json";
		if (args.ColumnCount() > 2) {
			auto ct = args.data[2].GetValue(i);
			if (!ct.IsNull()) {
				content_types[i] = ct.ToString();
			}
		}
		if (args.ColumnCount() > 3) {
			headers[i] = HeadersFromValue(args.data[3].GetValue(i));
		}
	}

	vector<QuackapiHttpFetchResult> out(count);
	FanOutRequests(count, skip, limits.fanout, out, [&](idx_t i) {
		return QuackapiHttpFetch::Post(db, urls[i], bodies[i], content_types[i], headers[i], limits);
	});

	for (idx_t i = 0; i < count; i++) {
		result.SetValue(i, skip[i] ? Value(FetchResultType()) : ToValue(out[i]));
	}
}

} // namespace

void RegisterQuackapiHttpFetchFunctions(ExtensionLoader &loader) {
	const auto header_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	const auto result_type = FetchResultType();

	// VOLATILE on every overload, deliberately. Without it DuckDB constant-folds
	// a call whose arguments are all literals — `FROM range(1000)` around a post
	// with a fixed URL executes ONE request and copies the answer 1000 times.
	// That is wrong for a function with side effects and it silently invalidates
	// any benchmark built on it.
	ScalarFunctionSet post_set("quackapi_post");
	ScalarFunction post2("quackapi_post", {LogicalType::VARCHAR, LogicalType::VARCHAR}, result_type, PostScalar);
	post2.stability = FunctionStability::VOLATILE;
	ScalarFunction post2j("quackapi_post", {LogicalType::VARCHAR, LogicalType::JSON()}, result_type, PostScalar);
	post2j.stability = FunctionStability::VOLATILE;
	ScalarFunction post3("quackapi_post", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                     result_type, PostScalar);
	post3.stability = FunctionStability::VOLATILE;
	ScalarFunction post4("quackapi_post",
	                     {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, header_type}, result_type,
	                     PostScalar);
	post4.stability = FunctionStability::VOLATILE;
	post_set.AddFunction(post2);
	post_set.AddFunction(post2j);
	post_set.AddFunction(post3);
	post_set.AddFunction(post4);
	loader.RegisterFunction(post_set);
}

} // namespace duckdb
