#include "quackapi_http_fetch.hpp"

#include <chrono>
#include <mutex>
#include <thread>

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
// Read/write: LLM upstreams are slow; the caller cancels. Connect stays short so
// a dead peer (or CI loopback after stop) fails fast instead of parking a worker.
constexpr time_t OUTBOUND_TIMEOUT_SECONDS = 600;
constexpr time_t OUTBOUND_CONNECT_TIMEOUT_SECONDS = 5;

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

//! Split "http://host:port" (from SplitURL origin) into host + port.
void SplitOriginHostPort(const string &origin, string &host, int &port) {
	host.clear();
	port = 80;
	string rest = origin;
	if (StringUtil::StartsWith(StringUtil::Lower(rest), "http://")) {
		rest = rest.substr(7);
	} else if (StringUtil::StartsWith(StringUtil::Lower(rest), "https://")) {
		rest = rest.substr(8);
		port = 443;
	}
	// Strip any leftover path (should not be present on an origin).
	auto slash = rest.find('/');
	if (slash != string::npos) {
		rest = rest.substr(0, slash);
	}
	auto colon = rest.rfind(':');
	if (colon != string::npos && rest.find(']') == string::npos) {
		// hostname:port (skip IPv6 bracket form for this test seam)
		host = rest.substr(0, colon);
		port = std::stoi(rest.substr(colon + 1));
	} else if (colon != string::npos && rest.front() == '[') {
		// [ipv6]:port
		auto br = rest.rfind(']');
		if (br != string::npos && br + 1 < rest.size() && rest[br + 1] == ':') {
			host = rest.substr(1, br - 1);
			port = std::stoi(rest.substr(br + 2));
		} else {
			host = rest;
		}
	} else {
		host = rest;
	}
}

//! Parse a raw HTTP/1.x response into status + body. Tolerates truncation.
void ParseRawHttpResponse(const string &raw, QuackapiHttpFetchResult &out) {
	auto header_end = raw.find("\r\n\r\n");
	string header_block;
	if (header_end == string::npos) {
		header_block = raw;
		out.body.clear();
	} else {
		header_block = raw.substr(0, header_end);
		out.body = raw.substr(header_end + 4);
	}
	auto line_end = header_block.find("\r\n");
	string status_line = line_end == string::npos ? header_block : header_block.substr(0, line_end);
	// "HTTP/1.1 200 OK"
	auto sp1 = status_line.find(' ');
	if (sp1 != string::npos) {
		auto sp2 = status_line.find(' ', sp1 + 1);
		string code = sp2 == string::npos ? status_line.substr(sp1 + 1) : status_line.substr(sp1 + 1, sp2 - sp1 - 1);
		try {
			int status = std::stoi(code);
			out.status = HTTPUtil::ToStatusCode(status);
			out.success = status >= 200 && status < 400;
			if (sp2 != string::npos) {
				out.reason = status_line.substr(sp2 + 1);
			}
		} catch (...) {
			out.request_error = "failed to parse HTTP status line";
		}
	} else if (!raw.empty()) {
		out.request_error = "malformed HTTP response";
	}
	if (line_end != string::npos) {
		idx_t pos = line_end + 2;
		while (pos < header_block.size()) {
			auto next = header_block.find("\r\n", pos);
			string line = next == string::npos ? header_block.substr(pos) : header_block.substr(pos, next - pos);
			auto colon = line.find(':');
			if (colon != string::npos) {
				string key = line.substr(0, colon);
				string val = line.substr(colon + 1);
				StringUtil::Trim(key);
				StringUtil::Trim(val);
				out.headers.Insert(key, val);
			}
			if (next == string::npos) {
				break;
			}
			pos = next + 2;
		}
	}
}

//! Plain HTTP GET that sends the request, waits stall_ms, then reads. Matches
//! a raw socket that stops reading — the only client shape that trips httplib
//! server write_timeout / ApplyRouteIoTimeout.
QuackapiHttpFetchResult PlainGetWithStall(const string &url, const unordered_map<string, string> &extra_headers,
                                          int32_t stall_ms) {
	QuackapiHttpFetchResult out;
	string origin, path;
	SplitURL(url, origin, path);
	string host;
	int port = 80;
	SplitOriginHostPort(origin, host, port);
	if (host.empty()) {
		out.request_error = "stall_ms: could not parse host from URL";
		return out;
	}

	duckdb_httplib::Error conn_error = duckdb_httplib::Error::Success;
	auto sock = duckdb_httplib::detail::create_client_socket(
	    host, "", port, AF_UNSPEC, /*tcp_nodelay=*/true, /*ipv6_v6only=*/false, duckdb_httplib::default_socket_options,
	    OUTBOUND_CONNECT_TIMEOUT_SECONDS, 0, OUTBOUND_TIMEOUT_SECONDS, 0, OUTBOUND_TIMEOUT_SECONDS, 0, /*intf=*/"",
	    conn_error);
	if (sock == INVALID_SOCKET) {
		out.request_error = "stall_ms connect: " + duckdb_httplib::to_string(conn_error);
		return out;
	}

	string req = "GET " + path + " HTTP/1.1\r\nHost: " + host;
	if (port != 80) {
		req += ":" + std::to_string(port);
	}
	req += "\r\nConnection: close\r\n";
	for (auto &kv : extra_headers) {
		req += kv.first + ": " + kv.second + "\r\n";
	}
	req += "\r\n";

	const char *send_ptr = req.data();
	size_t send_left = req.size();
	while (send_left > 0) {
		auto n = duckdb_httplib::detail::send_socket(sock, send_ptr, send_left, CPPHTTPLIB_SEND_FLAGS);
		if (n <= 0) {
			duckdb_httplib::detail::close_socket(sock);
			out.request_error = "stall_ms: send failed";
			return out;
		}
		send_ptr += n;
		send_left -= static_cast<size_t>(n);
	}

	if (stall_ms > 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
	}

	string raw;
	char buf[16384];
	while (true) {
		auto n = duckdb_httplib::detail::read_socket(sock, buf, sizeof(buf), CPPHTTPLIB_RECV_FLAGS);
		if (n <= 0) {
			break;
		}
		raw.append(buf, static_cast<size_t>(n));
	}
	duckdb_httplib::detail::close_socket(sock);

	if (raw.empty()) {
		out.request_error = "stall_ms: empty response (peer closed during stall)";
		return out;
	}
	ParseRawHttpResponse(raw, out);
	return out;
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
	client->set_connection_timeout(OUTBOUND_CONNECT_TIMEOUT_SECONDS, 0);
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
                                               const unordered_map<string, string> &extra_headers, int32_t stall_ms) {
	if (stall_ms < 0) {
		throw InvalidInputException("quackapi_fetch stall_ms must be >= 0");
	}
	if (stall_ms > 0) {
		if (!IsPlainHTTP(url)) {
			throw InvalidInputException("quackapi_fetch stall_ms requires an http:// URL (plain TCP stall seam)");
		}
		return PlainGetWithStall(url, extra_headers, stall_ms);
	}
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

int32_t StallMsFromArgs(DataChunk &args, idx_t row, idx_t col) {
	if (args.ColumnCount() <= col) {
		return 0;
	}
	auto v = args.data[col].GetValue(row);
	if (v.IsNull()) {
		return 0;
	}
	return v.GetValue<int32_t>();
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
		unordered_map<string, string> headers;
		int32_t stall_ms = 0;
		if (args.ColumnCount() == 2) {
			// Overload is either (url, headers MAP) or (url, stall_ms INTEGER).
			if (args.data[1].GetType().id() == LogicalTypeId::INTEGER ||
			    args.data[1].GetType().id() == LogicalTypeId::BIGINT) {
				stall_ms = StallMsFromArgs(args, i, 1);
			} else {
				headers = HeadersFromValue(args.data[1].GetValue(i));
			}
		} else if (args.ColumnCount() >= 3) {
			headers = HeadersFromValue(args.data[1].GetValue(i));
			stall_ms = StallMsFromArgs(args, i, 2);
		}
		result.SetValue(i, ToValue(QuackapiHttpFetch::Get(db, url.ToString(), headers, stall_ms)));
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

struct FetchTableBindData : public TableFunctionData {
	string url;
	unordered_map<string, string> headers;
	int32_t stall_ms = 0;
	bool finished = false;
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

//! Concurrent GETs — the seam SQLLogic needs for claim-race / fan-out tests.
//! `FROM range(n)` around scalar quackapi_fetch constant-folds to one request;
//! this table function fires n real threads and returns one row per attempt.
struct ParallelFetchBindData : public TableFunctionData {
	string url;
	int32_t n = 1;
};

struct ParallelFetchRow {
	int32_t idx = 0;
	int32_t status = 0;
	string body;
	string error;
};

struct ParallelFetchGlobalState : public GlobalTableFunctionState {
	vector<ParallelFetchRow> rows;
	idx_t offset = 0;
};

unique_ptr<FunctionData> ParallelFetchBind(ClientContext &, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<ParallelFetchBindData>();
	bind->url = input.inputs[0].GetValue<string>();
	bind->n = input.inputs[1].GetValue<int32_t>();
	if (bind->n < 1) {
		throw InvalidInputException("quackapi_parallel_fetch: n must be >= 1");
	}
	if (bind->n > 256) {
		throw InvalidInputException("quackapi_parallel_fetch: n max is 256");
	}
	names = {"idx", "status", "body", "error"};
	return_types = {LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> ParallelFetchInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ParallelFetchBindData>();
	auto state = make_uniq<ParallelFetchGlobalState>();
	state->rows.resize(NumericCast<idx_t>(bind.n));
	auto &db = *context.db;
	vector<std::thread> threads;
	threads.reserve(NumericCast<idx_t>(bind.n));
	for (int32_t i = 0; i < bind.n; i++) {
		threads.emplace_back([&db, &bind, &state, i]() {
			auto res = QuackapiHttpFetch::Get(db, bind.url);
			auto &row = state->rows[NumericCast<idx_t>(i)];
			row.idx = i;
			row.status = static_cast<int32_t>(res.status);
			row.body = std::move(res.body);
			row.error = std::move(res.request_error);
		});
	}
	for (auto &t : threads) {
		t.join();
	}
	return std::move(state);
}

void ParallelFetchExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<ParallelFetchGlobalState>();
	idx_t row = 0;
	while (state.offset < state.rows.size() && row < STANDARD_VECTOR_SIZE) {
		auto &r = state.rows[state.offset];
		output.SetValue(0, row, Value::INTEGER(r.idx));
		output.SetValue(1, row, Value::INTEGER(r.status));
		output.SetValue(2, row, Value(r.body));
		output.SetValue(3, row, r.error.empty() ? Value(LogicalType::VARCHAR) : Value(r.error));
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
	// stall_ms: after send, wait N ms before reading (socket write-timeout test seam).
	ScalarFunction fetch_stall("quackapi_fetch", {LogicalType::VARCHAR, LogicalType::INTEGER}, result_type,
	                           FetchScalar);
	fetch_stall.stability = FunctionStability::VOLATILE;
	ScalarFunction fetch_hdr_stall("quackapi_fetch", {LogicalType::VARCHAR, header_type, LogicalType::INTEGER},
	                               result_type, FetchScalar);
	fetch_hdr_stall.stability = FunctionStability::VOLATILE;
	fetch_set.AddFunction(fetch1);
	fetch_set.AddFunction(fetch2);
	fetch_set.AddFunction(fetch_stall);
	fetch_set.AddFunction(fetch_hdr_stall);
	loader.RegisterFunction(fetch_set);

	// Table form exposes named stall_ms := N (scalars have no named_parameters).
	TableFunction fetch_tf(
	    "quackapi_fetch", {LogicalType::VARCHAR},
	    [](ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
		    auto &bind = data_p.bind_data->CastNoConst<FetchTableBindData>();
		    if (bind.finished) {
			    return;
		    }
		    auto result = QuackapiHttpFetch::Get(*context.db, bind.url, bind.headers, bind.stall_ms);
		    auto value = ToValue(result);
		    auto &children = StructValue::GetChildren(value);
		    for (idx_t c = 0; c < children.size(); c++) {
			    output.SetValue(c, 0, children[c]);
		    }
		    output.SetCardinality(1);
		    bind.finished = true;
	    },
	    [](ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
	       vector<string> &names) -> unique_ptr<FunctionData> {
		    auto bind = make_uniq<FetchTableBindData>();
		    if (input.inputs.empty() || input.inputs[0].IsNull()) {
			    throw InvalidInputException("quackapi_fetch(url, stall_ms := N): url must be non-NULL");
		    }
		    bind->url = input.inputs[0].ToString();
		    auto headers_it = input.named_parameters.find("headers");
		    if (headers_it != input.named_parameters.end() && !headers_it->second.IsNull()) {
			    bind->headers = HeadersFromValue(headers_it->second);
		    }
		    auto stall_it = input.named_parameters.find("stall_ms");
		    if (stall_it != input.named_parameters.end() && !stall_it->second.IsNull()) {
			    bind->stall_ms = stall_it->second.GetValue<int32_t>();
		    }
		    auto result_type = FetchResultType();
		    for (auto &child : StructType::GetChildTypes(result_type)) {
			    names.push_back(child.first);
			    return_types.push_back(child.second);
		    }
		    return std::move(bind);
	    });
	fetch_tf.named_parameters["headers"] = header_type;
	fetch_tf.named_parameters["stall_ms"] = LogicalType::INTEGER;
	loader.RegisterFunction(fetch_tf);

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

	TableFunction parallel_fetch("quackapi_parallel_fetch", {LogicalType::VARCHAR, LogicalType::INTEGER},
	                             ParallelFetchExec, ParallelFetchBind, ParallelFetchInit);
	loader.RegisterFunction(parallel_fetch);
}

} // namespace duckdb
