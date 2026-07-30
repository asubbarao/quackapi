#include "quackapi_http_fetch.hpp"

#include <mutex>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/client_context.hpp"

#include "httplib.hpp"

namespace duckdb {

namespace {

constexpr idx_t MAX_IDLE_PER_HOST = 64;
constexpr time_t OUTBOUND_TIMEOUT_SECONDS = 600; // LLM upstreams are slow; the caller cancels.

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
	idx_t dialed = 0;
	idx_t reused = 0;
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

unique_ptr<duckdb_httplib::Client> DialPlain(const string &origin) {
	auto client = make_uniq<duckdb_httplib::Client>(origin);
	client->set_keep_alive(true);
	// Same Nagle/delayed-ACK deadlock the server side hit (quackapi_server.cpp:917),
	// just on the other end of the socket: without this a small POST body waits on
	// the peer's delayed ACK before the request is even complete.
	client->set_tcp_nodelay(true);
	client->set_follow_location(true);
	client->set_decompress(true);
	client->set_read_timeout(OUTBOUND_TIMEOUT_SECONDS, 0);
	client->set_write_timeout(OUTBOUND_TIMEOUT_SECONDS, 0);
	client->set_connection_timeout(30, 0);
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
QuackapiHttpFetchResult WithPlainClient(const string &origin, CALL &&call) {
	unique_ptr<duckdb_httplib::Client> client;
	bool reused = false;
	{
		auto &pool = ConnectionPool::Get();
		std::lock_guard<std::mutex> guard(pool.lock);
		auto &host = pool.hosts[origin];
		if (!host.plain_idle.empty()) {
			client = std::move(host.plain_idle.back().client);
			host.plain_idle.pop_back();
			host.reused++;
			reused = true;
		} else {
			host.dialed++;
		}
	}
	if (!client) {
		client = DialPlain(origin);
	}

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
		if (host.plain_idle.size() < MAX_IDLE_PER_HOST) {
			host.plain_idle.push_back(PooledPlain {std::move(client)});
		}
	}
	return result;
}

//! Same checkout/return dance for the HTTPUtil path (https, or a loaded
//! curl_httpfs the operator wants used). The two-argument HTTPUtil::Request
//! reuses the client we pass instead of constructing a throwaway one.
template <class BUILD>
QuackapiHttpFetchResult WithUtilClient(DatabaseInstance &db, const string &url, BUILD &&build) {
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
			host.reused++;
			reused = true;
		} else {
			host.dialed++;
		}
	}

	auto params = http_util.InitializeParameters(db, url);
	params->keep_alive = true;
	auto result = build(http_util, *params, client);
	result.reused_connection = reused;

	if (client && result.request_error.empty()) {
		auto &pool = ConnectionPool::Get();
		std::lock_guard<std::mutex> guard(pool.lock);
		auto &host = pool.hosts[key];
		if (host.util_idle.size() < MAX_IDLE_PER_HOST) {
			host.util_idle.push_back(PooledUtil {std::move(client)});
		}
	}
	return result;
}

} // namespace

//===--------------------------------------------------------------------===//
// Public API
//===--------------------------------------------------------------------===//

string QuackapiHttpFetch::ActiveHttpUtilName(DatabaseInstance &db) {
	return HTTPUtil::Get(db).GetName();
}

QuackapiHttpFetchResult QuackapiHttpFetch::Get(DatabaseInstance &db, const string &url,
                                               const unordered_map<string, string> &extra_headers) {
	if (IsPlainHTTP(url)) {
		string origin, path;
		SplitURL(url, origin, path);
		auto headers = ToHttplibHeaders(extra_headers);
		return WithPlainClient(origin, [&](duckdb_httplib::Client &client) { return client.Get(path, headers); });
	}

	return WithUtilClient(db, url, [&](HTTPUtil &http_util, HTTPParams &params, unique_ptr<HTTPClient> &client) {
		HTTPHeaders headers(db);
		InsertExtraHeaders(headers, extra_headers);
		GetRequestInfo request(url, headers, params, /*response_handler=*/nullptr,
		                       /*content_handler=*/nullptr);
		request.try_request = true;
		return FromResponse(http_util.Request(request, client));
	});
}

QuackapiHttpFetchResult QuackapiHttpFetch::Post(DatabaseInstance &db, const string &url, const string &body,
                                                const string &content_type,
                                                const unordered_map<string, string> &extra_headers) {
	if (IsPlainHTTP(url)) {
		// The vendored httplib implements POST, so plain HTTP needs no companion
		// extension at all — unlike the HTTPUtil path below.
		string origin, path;
		SplitURL(url, origin, path);
		auto headers = ToHttplibHeaders(extra_headers);
		const auto &ct = content_type.empty() ? string("application/octet-stream") : content_type;
		return WithPlainClient(origin,
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

	return WithUtilClient(db, url, [&](HTTPUtil &http_util, HTTPParams &params, unique_ptr<HTTPClient> &client) {
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

vector<QuackapiHttpPoolStats> QuackapiHttpFetch::PoolStats() {
	vector<QuackapiHttpPoolStats> out;
	auto &pool = ConnectionPool::Get();
	std::lock_guard<std::mutex> guard(pool.lock);
	for (auto &entry : pool.hosts) {
		QuackapiHttpPoolStats stats;
		stats.host = entry.first;
		stats.idle = entry.second.plain_idle.size() + entry.second.util_idle.size();
		stats.dialed = entry.second.dialed;
		stats.reused = entry.second.reused;
		out.push_back(std::move(stats));
	}
	return out;
}

void QuackapiHttpFetch::ResetPool() {
	auto &pool = ConnectionPool::Get();
	std::lock_guard<std::mutex> guard(pool.lock);
	pool.hosts.clear();
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

void FetchScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &db = *state.GetContext().db;
	const auto count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; i++) {
		auto url = args.data[0].GetValue(i);
		if (url.IsNull()) {
			result.SetValue(i, Value(FetchResultType()));
			continue;
		}
		auto headers =
		    args.ColumnCount() > 1 ? HeadersFromValue(args.data[1].GetValue(i)) : unordered_map<string, string>();
		result.SetValue(i, ToValue(QuackapiHttpFetch::Get(db, url.ToString(), headers)));
	}
}

void PostScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &db = *state.GetContext().db;
	const auto count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t i = 0; i < count; i++) {
		auto url = args.data[0].GetValue(i);
		auto body = args.data[1].GetValue(i);
		if (url.IsNull()) {
			result.SetValue(i, Value(FetchResultType()));
			continue;
		}
		string content_type = "application/json";
		if (args.ColumnCount() > 2) {
			auto ct = args.data[2].GetValue(i);
			if (!ct.IsNull()) {
				content_type = ct.ToString();
			}
		}
		auto headers =
		    args.ColumnCount() > 3 ? HeadersFromValue(args.data[3].GetValue(i)) : unordered_map<string, string>();
		result.SetValue(i, ToValue(QuackapiHttpFetch::Post(
		                       db, url.ToString(), body.IsNull() ? string() : body.ToString(), content_type, headers)));
	}
}

struct PoolGlobalState : public GlobalTableFunctionState {
	vector<QuackapiHttpPoolStats> rows;
	idx_t offset = 0;
};

unique_ptr<FunctionData> PoolBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                  vector<string> &names) {
	names = {"host", "idle", "dialed", "reused"};
	return_types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT};
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> PoolInit(ClientContext &, TableFunctionInitInput &) {
	auto state = make_uniq<PoolGlobalState>();
	state->rows = QuackapiHttpFetch::PoolStats();
	return std::move(state);
}

void PoolExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<PoolGlobalState>();
	idx_t row = 0;
	while (state.offset < state.rows.size() && row < STANDARD_VECTOR_SIZE) {
		auto &entry = state.rows[state.offset];
		output.SetValue(0, row, Value(entry.host));
		output.SetValue(1, row, Value::BIGINT(NumericCast<int64_t>(entry.idle)));
		output.SetValue(2, row, Value::BIGINT(NumericCast<int64_t>(entry.dialed)));
		output.SetValue(3, row, Value::BIGINT(NumericCast<int64_t>(entry.reused)));
		row++;
		state.offset++;
	}
	output.SetCardinality(row);
}

} // namespace

void RegisterQuackapiHttpFetchFunctions(ExtensionLoader &loader) {
	const auto header_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	const auto result_type = FetchResultType();

	// VOLATILE on every overload, deliberately. Without it DuckDB constant-folds
	// a call whose arguments are all literals — `FROM range(1000)` around a fetch
	// with a fixed URL executes ONE request and copies the answer 1000 times.
	// That is wrong for a function with side effects and it silently invalidates
	// any benchmark built on it.
	ScalarFunctionSet fetch_set("quackapi_fetch");
	ScalarFunction fetch1("quackapi_fetch", {LogicalType::VARCHAR}, result_type, FetchScalar);
	fetch1.stability = FunctionStability::VOLATILE;
	ScalarFunction fetch2("quackapi_fetch", {LogicalType::VARCHAR, header_type}, result_type, FetchScalar);
	fetch2.stability = FunctionStability::VOLATILE;
	fetch_set.AddFunction(fetch1);
	fetch_set.AddFunction(fetch2);
	loader.RegisterFunction(fetch_set);

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

	loader.RegisterFunction(TableFunction("quackapi_http_pool", {}, PoolExec, PoolBind, PoolInit));
}

} // namespace duckdb
