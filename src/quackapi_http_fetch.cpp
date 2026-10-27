#include "quackapi_http_fetch.hpp"
#include "quackapi_limits.hpp"
#include "quackapi_util.hpp"
#include "quackapi_server.hpp"
#include "quackapi_state.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parallel/async_result.hpp"
#include "duckdb/parallel/callback_async_task.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/parallel/task.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/function/table/arrow/arrow_duck_schema.hpp"

#include "duckdb.h"
#include "nanoarrow/nanoarrow_ipc.h"

#include "httplib.hpp"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

namespace {

enum class ResponseFormat : uint8_t { RAW, ARROW, PARQUET };

ResponseFormat ParseResponseFormat(const string &format) {
	if (format.empty() || StringUtil::Lower(format) == "raw") {
		return ResponseFormat::RAW;
	}
	if (StringUtil::Lower(format) == "arrow") {
		return ResponseFormat::ARROW;
	}
	if (StringUtil::Lower(format) == "parquet") {
		return ResponseFormat::PARQUET;
	}
	throw InvalidInputException("response_format must be raw, arrow, or parquet");
}

string ExpectedResponseType(ResponseFormat format) {
	return format == ResponseFormat::ARROW ? "application/vnd.apache.arrow.stream" : "application/vnd.apache.parquet";
}

void ApplyResponseAccept(unordered_map<string, string> &headers, ResponseFormat format) {
	if (format == ResponseFormat::RAW) {
		return;
	}
	const auto expected = ExpectedResponseType(format);
	for (auto &header : headers) {
		if (StringUtil::Lower(header.first) == "accept") {
			if (StringUtil::Lower(header.second).find(expected) == string::npos) {
				throw InvalidInputException("response_format conflicts with the supplied Accept header");
			}
			return;
		}
	}
	headers["Accept"] = expected;
}

bool HasResponseType(const HTTPHeaders &headers, ResponseFormat format) {
	const auto expected = ExpectedResponseType(format);
	for (auto &header : headers) {
		if (StringUtil::Lower(header.first) == "content-type") {
			return StringUtil::Lower(header.second).find(expected) == 0;
		}
	}
	return false;
}

vector<string> ArrowColumnNames(const ArrowSchema &schema) {
	vector<string> names;
	for (int64_t i = 0; i < schema.n_children; i++) {
		auto child = schema.children[i];
		names.emplace_back(child && child->name ? child->name : "column_" + to_string(i + 1));
	}
	return names;
}

void CheckTypedDecodeDeadline(ClientContext &context, std::chrono::steady_clock::time_point deadline) {
	context.BlockingInterruptCheck();
	if (std::chrono::steady_clock::now() >= deadline) {
		throw InvalidInputException("typed response decoding exceeded the request deadline");
	}
}

QuackapiTypedResult TypedRowsFromArrowBody(ClientContext &context, DatabaseInstance &db, const string &body,
                                           idx_t max_bytes, std::chrono::steady_clock::time_point deadline) {
	ArrowBuffer buffer;
	ArrowBufferInit(&buffer);
	auto buffer_error = ArrowBufferAppend(&buffer, reinterpret_cast<const uint8_t *>(body.data()), body.size());
	if (buffer_error != NANOARROW_OK) {
		ArrowBufferReset(&buffer);
		throw InvalidInputException("could not buffer Arrow IPC response");
	}
	ArrowIpcInputStream input {};
	auto input_error = ArrowIpcInputStreamInitBuffer(&input, &buffer);
	if (input_error != NANOARROW_OK) {
		ArrowBufferReset(&buffer);
		throw InvalidInputException("could not initialize Arrow IPC input");
	}
	ArrowArrayStream stream {};
	ArrowIpcArrayStreamReaderOptions options {};
	options.field_index = -1;
	options.use_shared_buffers = 0;
	auto stream_error = ArrowIpcArrayStreamReaderInit(&stream, &input, &options);
	if (stream_error != NANOARROW_OK) {
		if (input.release) {
			input.release(&input);
		}
		throw InvalidInputException("invalid Arrow IPC response");
	}
	auto stream_guard =
	    unique_ptr<ArrowArrayStream, std::function<void(ArrowArrayStream *)>>(&stream, [](ArrowArrayStream *s) {
		    if (s->release)
			    s->release(s);
	    });
	ArrowSchema schema {};
	if (stream.get_schema(&stream, &schema) != 0) {
		throw InvalidInputException("could not read Arrow IPC schema");
	}
	auto schema_guard = unique_ptr<ArrowSchema, std::function<void(ArrowSchema *)>>(&schema, [](ArrowSchema *s) {
		if (s->release)
			s->release(s);
	});
	auto names = ArrowColumnNames(schema);
	Connection connection(db);
	const auto c_connection = reinterpret_cast<duckdb_connection>(&connection);
	duckdb_arrow_converted_schema converted = nullptr;
	auto schema_error = duckdb_schema_from_arrow(c_connection, &schema, &converted);
	if (schema_error) {
		string message = duckdb_error_data_message(schema_error) ? duckdb_error_data_message(schema_error)
		                                                         : "schema conversion failed";
		duckdb_destroy_error_data(&schema_error);
		throw InvalidInputException("Arrow schema conversion failed: %s", message);
	}
	auto converted_guard =
	    unique_ptr<duckdb_arrow_converted_schema, std::function<void(duckdb_arrow_converted_schema *)>>(
	        &converted, [](duckdb_arrow_converted_schema *s) {
		        if (*s)
			        duckdb_destroy_arrow_converted_schema(s);
	        });
	auto converted_schema = reinterpret_cast<ArrowTableSchema *>(converted);
	QuackapiTypedRowCollector collector(names, converted_schema->GetTypes(), max_bytes);
	for (;;) {
		CheckTypedDecodeDeadline(context, deadline);
		ArrowArray array {};
		if (stream.get_next(&stream, &array) != 0) {
			if (array.release)
				array.release(&array);
			throw InvalidInputException("could not decode Arrow IPC record batch");
		}
		if (!array.release) {
			break;
		}
		duckdb_data_chunk native_chunk = nullptr;
		auto array_error = duckdb_data_chunk_from_arrow(c_connection, &array, converted, &native_chunk);
		if (array_error) {
			string message = duckdb_error_data_message(array_error) ? duckdb_error_data_message(array_error)
			                                                        : "array conversion failed";
			duckdb_destroy_error_data(&array_error);
			if (array.release)
				array.release(&array);
			throw InvalidInputException("Arrow array conversion failed: %s", message);
		}
		auto chunk_guard = unique_ptr<duckdb_data_chunk, std::function<void(duckdb_data_chunk *)>>(
		    &native_chunk, [](duckdb_data_chunk *c) {
			    if (*c)
				    duckdb_destroy_data_chunk(c);
		    });
		auto &chunk = *reinterpret_cast<DataChunk *>(native_chunk);
		for (idx_t row = 0; row < chunk.size(); row++) {
			if ((row & 255) == 0)
				CheckTypedDecodeDeadline(context, deadline);
			vector<Value> values;
			values.reserve(chunk.ColumnCount());
			for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
				values.emplace_back(chunk.GetValue(col, row));
			}
			collector.AppendRow(std::move(values));
		}
	}
	return collector.Finish();
}

string ParquetTempPath(FileSystem &fs) {
	string directory;
#if defined(_WIN32) || defined(WIN32)
	const char *temp = std::getenv("TEMP");
	if (!temp || !temp[0])
		temp = std::getenv("TMP");
	directory = (temp && temp[0]) ? string(temp) : string(".");
#else
	const char *temp = std::getenv("TMPDIR");
	directory = (temp && temp[0]) ? string(temp) : string("/tmp");
#endif
	auto uid = StringUtil::Replace(UUID::ToString(UUIDv7::GenerateRandomUUID()), "-", "");
	return StringUtil::Replace(fs.JoinPath(directory, "quackapi_post_" + uid + ".parquet"), "\\", "/");
}

QuackapiTypedResult TypedRowsFromParquetBody(ClientContext &context, const string &body, idx_t max_bytes,
                                             std::chrono::steady_clock::time_point deadline) {
	if (body.size() > max_bytes) {
		throw InvalidInputException("Parquet response exceeds quackapi_max_response_bytes");
	}
	Connection connection(*context.db);
	QuackapiQueryDeadline query_deadline(connection, deadline, &context);
	auto &fs = FileSystem::GetFileSystem(*connection.context);
	auto path = ParquetTempPath(fs);
	struct Cleanup {
		FileSystem &fs;
		string path;
		~Cleanup() {
			fs.TryRemoveFile(path);
		}
	} cleanup {fs, path};
	auto file = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
	if (!body.empty()) {
		const auto written = file->Write(const_cast<char *>(body.data()), body.size());
		if (written != NumericCast<int64_t>(body.size())) {
			throw IOException("short write while staging Parquet response for native decode");
		}
	}
	file->Close();
	auto result = connection.Submit("SELECT * FROM read_parquet('" + StringUtil::Replace(path, "'", "''") + "')");
	if (!result) {
		throw InvalidInputException("Parquet response decode failed: no result");
	}
	QueryResultStream<ChunkFormat> stream(std::move(result));
	CheckTypedDecodeDeadline(context, deadline);
	auto chunk = stream.Fetch();
	QuackapiTypedRowCollector collector(QuackapiNames(stream.GetNames()), stream.GetTypes(), max_bytes);
	while (chunk) {
		CheckTypedDecodeDeadline(context, deadline);
		collector.AppendChunk(*chunk);
		chunk = stream.Fetch();
	}
	return collector.Finish();
}

thread_local ClientContext *outbound_context = nullptr;
struct ScopedOutboundContext {
	explicit ScopedOutboundContext(ClientContext &context) : previous(outbound_context) {
		outbound_context = &context;
	}
	~ScopedOutboundContext() {
		outbound_context = previous;
	}
	ClientContext *previous;
};

constexpr idx_t MAX_IDLE_PER_HOST = 64;
constexpr idx_t MAX_IDLE_TOTAL = 256;
constexpr idx_t MAX_POOL_HOSTS = 256;
constexpr auto IDLE_POOL_TTL = std::chrono::minutes(5);
// Concurrency cap when one chunk carries many independent calls. Kept at or below
// MAX_IDLE_PER_HOST so a fan-out cannot dial more sockets than the pool will keep,
// which would turn every burst into a churn of one-shot connections.
constexpr idx_t MAX_OUTBOUND_FANOUT = 32;
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
	unique_ptr<quackapi_httplib::Client> client;
};

struct PooledUtil {
	unique_ptr<HTTPClient> client;
};

struct HostPool {
	vector<PooledPlain> plain_idle;
	vector<PooledUtil> util_idle;
	idx_t dialed = 0;
	idx_t reused = 0;
	std::chrono::steady_clock::time_point last_used = std::chrono::steady_clock::now();
};

struct ConnectionPool {
	static ConnectionPool &Get() {
		static ConnectionPool instance;
		return instance;
	}

	std::mutex lock;
	unordered_map<string, HostPool> hosts;
};

// The caller holds pool.lock. Eviction also bounds metadata for one-off hosts.
HostPool &PoolHost(ConnectionPool &pool, const string &key) {
	const auto now = std::chrono::steady_clock::now();
	for (auto it = pool.hosts.begin(); it != pool.hosts.end();) {
		if (now - it->second.last_used > IDLE_POOL_TTL)
			it = pool.hosts.erase(it);
		else
			++it;
	}
	if (pool.hosts.find(key) == pool.hosts.end() && pool.hosts.size() >= MAX_POOL_HOSTS) {
		auto oldest = pool.hosts.begin();
		for (auto it = pool.hosts.begin(); it != pool.hosts.end(); ++it) {
			if (it->second.last_used < oldest->second.last_used)
				oldest = it;
		}
		pool.hosts.erase(oldest);
	}
	auto &host = pool.hosts[key];
	host.last_used = now;
	return host;
}

idx_t TotalIdleClients(const ConnectionPool &pool) {
	idx_t count = 0;
	for (const auto &entry : pool.hosts)
		count += entry.second.plain_idle.size() + entry.second.util_idle.size();
	return count;
}

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

unique_ptr<quackapi_httplib::Client> DialPlain(const string &origin) {
	auto client = make_uniq<quackapi_httplib::Client>(origin);
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

quackapi_httplib::Headers ToHttplibHeaders(const unordered_map<string, string> &extra) {
	quackapi_httplib::Headers headers;
	for (auto &kv : extra) {
		headers.emplace(kv.first, kv.second);
	}
	return headers;
}

QuackapiHttpFetchResult FromHttplibResult(const quackapi_httplib::Result &res) {
	QuackapiHttpFetchResult out;
	if (res.error() != quackapi_httplib::Error::Success) {
		out.request_error = quackapi_httplib::to_string(res.error());
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
	unique_ptr<quackapi_httplib::Client> client;
	bool reused = false;
	{
		auto &pool = ConnectionPool::Get();
		std::lock_guard<std::mutex> guard(pool.lock);
		auto &host = PoolHost(pool, origin);
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
	// Reset on every checkout: a previous request's short deadline must not leak.
	const auto timeout_ms = QuackapiRemainingTimeoutMillis(OUTBOUND_TIMEOUT_SECONDS * 1000);
	client->set_max_timeout(static_cast<time_t>(timeout_ms));
	client->set_read_timeout(static_cast<time_t>(timeout_ms / 1000), static_cast<time_t>((timeout_ms % 1000) * 1000));
	client->set_write_timeout(static_cast<time_t>(timeout_ms / 1000), static_cast<time_t>((timeout_ms % 1000) * 1000));
	const auto connect_ms = std::min<int64_t>(timeout_ms, OUTBOUND_CONNECT_TIMEOUT_SECONDS * 1000);
	client->set_connection_timeout(static_cast<time_t>(connect_ms / 1000),
	                               static_cast<time_t>((connect_ms % 1000) * 1000));

	QuackapiHttpFetchResult result;
	bool healthy = false;
	try {
		unique_ptr<QuackapiCancellationGuard> cancellation;
		if (outbound_context) {
			cancellation = make_uniq<QuackapiCancellationGuard>(*outbound_context, [&client]() { client->stop(); });
		}
		auto res = call(*client);
		healthy =
		    res.error() == quackapi_httplib::Error::Success && !(outbound_context && outbound_context->IsInterrupted());
		result = FromHttplibResult(res);
	} catch (...) {
		// Drop the client on the floor; a half-written socket is not reusable.
		throw;
	}
	result.reused_connection = reused;

	if (healthy) {
		auto &pool = ConnectionPool::Get();
		std::lock_guard<std::mutex> guard(pool.lock);
		auto &host = PoolHost(pool, origin);
		if (host.plain_idle.size() < MAX_IDLE_PER_HOST && TotalIdleClients(pool) < MAX_IDLE_TOTAL) {
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
		auto &host = PoolHost(pool, key);
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
	const auto timeout_ms = QuackapiRemainingTimeoutMillis(OUTBOUND_TIMEOUT_SECONDS * 1000);
	params->timeout = timeout_ms / 1000;
	params->timeout_usec = (timeout_ms % 1000) * 1000;
	auto result = build(http_util, *params, client);
	result.reused_connection = reused;

	if (client && result.request_error.empty()) {
		auto &pool = ConnectionPool::Get();
		std::lock_guard<std::mutex> guard(pool.lock);
		auto &host = PoolHost(pool, key);
		if (host.util_idle.size() < MAX_IDLE_PER_HOST && TotalIdleClients(pool) < MAX_IDLE_TOTAL) {
			host.util_idle.push_back(PooledUtil {std::move(client)});
		}
	}
	return result;
}

} // namespace

//===--------------------------------------------------------------------===//
// Public API
//===--------------------------------------------------------------------===//

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
		                       [&](quackapi_httplib::Client &client) { return client.Post(path, headers, body, ct); });
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
	children.emplace_back("body_blob", LogicalType::BLOB);
	child_list_t<LogicalType> column_fields;
	column_fields.emplace_back("name", LogicalType::VARCHAR);
	column_fields.emplace_back("type", LogicalType::TYPE());
	children.emplace_back("columns", LogicalType::LIST(LogicalType::STRUCT(std::move(column_fields))));
	children.emplace_back("rows", LogicalType::LIST(LogicalType::VARIANT()));
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
	children.emplace_back("body", Utf8Proc::IsValid(result.body.data(), result.body.size())
	                                  ? Value(result.body)
	                                  : Value(LogicalType::VARCHAR));
	children.emplace_back("headers", Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(header_keys),
	                                            std::move(header_values)));
	children.emplace_back("error",
	                      result.request_error.empty() ? Value(LogicalType::VARCHAR) : Value(result.request_error));
	children.emplace_back("reused_connection", Value::BOOLEAN(result.reused_connection));
	children.emplace_back("body_blob", Value::BLOB_RAW(result.body));
	children.emplace_back("columns", result.typed_result.columns);
	children.emplace_back("rows", result.typed_result.rows);
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
//! Why this exists: these are scalar functions, so a chunk can carry up to
//! STANDARD_VECTOR_SIZE independent requests. Run serially, N calls against an
//! upstream with latency L cost N*L while the machine idles and the connection
//! pool sits unused. The pool already tolerates concurrent checkout (every path
//! takes ConnectionPool::lock), so fanning out is the whole fix.
//!
//! `perform(i)` must be safe to call from any thread. Exceptions are captured per
//! row rather than allowed to escape: WithPlainClient deliberately rethrows on a
//! half-written socket. Native tasks return failures as receipts rather than
//! cancelling unrelated rows. Argument validation that
//! must still raise to the caller therefore happens BEFORE the fan-out.
//!
//! The single-request case — what a route handler doing one upstream call per
//! request looks like — stays fully inline. No thread is created for it.
//! The caller's deadline, tightened by the session's quackapi_http_timeout_ms. Outside a served
//! request there is no other deadline, so this is what stops a call to a peer that went silent.
//! The deadline is read from the connection too: a call evaluated on a DuckDB worker thread
//! (a parallel scan) never sees the request thread's deadline.
std::chrono::steady_clock::time_point OutboundDeadline(ClientContext &context, int64_t call_timeout_ms = 0) {
	auto deadline = QuackapiContextDeadline(context);
	Value setting;
	if (call_timeout_ms > 0 ||
	    (context.TryGetCurrentSetting("quackapi_http_timeout_ms", setting) && !setting.IsNull())) {
		const auto timeout_ms = call_timeout_ms > 0 ? call_timeout_ms : setting.GetValue<int64_t>();
		if (timeout_ms <= 0) {
			throw InvalidInputException("quackapi_http_timeout_ms must be > 0");
		}
		deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
	}
	return deadline;
}

//! Restores the calling thread's deadline when an inline request returns or throws.
struct ScopedOutboundDeadline {
	explicit ScopedOutboundDeadline(std::chrono::steady_clock::time_point deadline)
	    : previous(QuackapiCurrentDeadline()) {
		QuackapiSetCurrentDeadline(deadline);
	}
	explicit ScopedOutboundDeadline(ClientContext &context) : ScopedOutboundDeadline(OutboundDeadline(context)) {
	}
	~ScopedOutboundDeadline() {
		QuackapiSetCurrentDeadline(previous);
	}
	std::chrono::steady_clock::time_point previous;
};

//! Scalar callbacks cannot yield a pipeline. Their compatibility adapter uses
//! the engine's async pool and participates in its drain instead of owning OS threads.
class OutboundExecutorTask : public BaseExecutorTask {
public:
	OutboundExecutorTask(TaskExecutor &executor, std::function<void()> work)
	    : BaseExecutorTask(executor), work(std::move(work)) {
	}
	void ExecuteTask() override {
		work();
	}
	string TaskType() const override {
		return "QuackapiOutbound";
	}

private:
	std::function<void()> work;
};

template <class PERFORM>
void FanOutRequests(ClientContext &context, idx_t count, const vector<bool> &skip, vector<QuackapiHttpFetchResult> &out,
                    PERFORM &&perform) {
	context.BlockingInterruptCheck();
	ScopedOutboundDeadline scoped(QuackapiContextDeadline(context));
	const auto deadline = QuackapiCurrentDeadline();
	auto run_one = [&](idx_t i) {
		ScopedOutboundDeadline scoped_row(deadline);
		ScopedOutboundContext context_scope(context);
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
		context.BlockingInterruptCheck();
		return;
	}

	const idx_t n_tasks = active < MAX_OUTBOUND_FANOUT ? active : MAX_OUTBOUND_FANOUT;
	std::atomic<idx_t> cursor {0};
	TaskExecutor executor(context, TaskSchedulerType::ASYNC);
	for (idx_t t = 0; t < n_tasks; t++) {
		executor.ScheduleTask(make_uniq<OutboundExecutorTask>(executor, [&]() {
			for (;;) {
				context.BlockingInterruptCheck();
				const idx_t i = cursor.fetch_add(1);
				if (i >= count) {
					return;
				}
				if (!skip[i]) {
					run_one(i);
				}
			}
		}));
	}
	executor.WorkOnTasks();
	context.BlockingInterruptCheck();
}

int64_t PositiveSetting(ClientContext &context, const string &name, int64_t fallback);
void DecodePostResponse(ClientContext &context, DatabaseInstance &db, QuackapiHttpFetchResult &response,
                        ResponseFormat format, std::chrono::steady_clock::time_point deadline);

void PostScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &db = *state.GetContext().db;
	const auto count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);

	vector<bool> skip(count, false);
	vector<string> urls(count);
	vector<string> bodies(count);
	vector<string> content_types(count);
	vector<ResponseFormat> response_formats(count, ResponseFormat::RAW);
	vector<unordered_map<string, string>> headers(count);
	vector<std::chrono::steady_clock::time_point> deadlines(count);
	const auto positional_count = args.ColumnCount() - 2;
	for (idx_t i = 0; i < count; i++) {
		auto url = args.data[0].GetValue(i);
		if (url.IsNull()) {
			skip[i] = true;
			continue;
		}
		// Keep positional NULL behavior while the optional timeout defaults to NULL.
		for (idx_t col = 1; col < positional_count; col++) {
			if (args.data[col].GetValue(i).IsNull()) {
				skip[i] = true;
				break;
			}
		}
		if (skip[i]) {
			continue;
		}
		auto response_format = args.data[args.ColumnCount() - 2].GetValue(i);
		response_formats[i] = ParseResponseFormat(response_format.IsNull() ? "raw" : response_format.ToString());
		auto timeout = args.data[args.ColumnCount() - 1].GetValue(i);
		const auto call_timeout_ms = timeout.IsNull() ? 0 : timeout.GetValue<int64_t>();
		if (!timeout.IsNull() && call_timeout_ms <= 0) {
			throw InvalidInputException("timeout_ms must be > 0");
		}
		deadlines[i] = OutboundDeadline(state.GetContext(), call_timeout_ms);
		urls[i] = url.ToString();
		auto body = args.data[1].GetValue(i);
		bodies[i] = body.IsNull() ? string() : body.ToString();
		content_types[i] = "application/json";
		if (positional_count > 2) {
			auto ct = args.data[2].GetValue(i);
			if (!ct.IsNull()) {
				content_types[i] = ct.ToString();
			}
		}
		if (positional_count > 3) {
			headers[i] = HeadersFromValue(args.data[3].GetValue(i));
		}
		ApplyResponseAccept(headers[i], response_formats[i]);
	}

	vector<QuackapiHttpFetchResult> out(count);
	FanOutRequests(state.GetContext(), count, skip, out, [&](idx_t i) {
		ScopedOutboundDeadline per_call(deadlines[i]);
		auto response = QuackapiHttpFetch::Post(db, urls[i], bodies[i], content_types[i], headers[i]);
		DecodePostResponse(state.GetContext(), db, response, response_formats[i], deadlines[i]);
		return response;
	});

	for (idx_t i = 0; i < count; i++) {
		result.SetValue(i, skip[i] ? Value(FetchResultType()) : ToValue(out[i]));
	}
}

int64_t PositiveSetting(ClientContext &context, const string &name, int64_t fallback) {
	Value setting;
	if (!context.TryGetCurrentSetting(Identifier(name), setting) || setting.IsNull()) {
		return fallback;
	}
	const auto value = setting.GetValue<int64_t>();
	if (value <= 0) {
		throw InvalidInputException("%s must be > 0", name);
	}
	return value;
}

void DecodePostResponse(ClientContext &context, DatabaseInstance &db, QuackapiHttpFetchResult &response,
                        ResponseFormat format, std::chrono::steady_clock::time_point deadline) {
	if (format == ResponseFormat::RAW || static_cast<int>(response.status) < 200 ||
	    static_cast<int>(response.status) >= 300) {
		return;
	}
	try {
		if (!HasResponseType(response.headers, format)) {
			throw InvalidInputException("response Content-Type does not match response_format");
		}
		int64_t serving_timeout = 0, response_limit = 0;
		if (!QuackapiGetServingBudget(context, serving_timeout, response_limit)) {
			response_limit = PositiveSetting(context, "quackapi_max_response_bytes", 16 * 1024 * 1024);
		}
		const auto max_bytes = NumericCast<idx_t>(response_limit);
		if (response.body.size() > max_bytes) {
			throw InvalidInputException("response body exceeds quackapi_max_response_bytes");
		}
		auto typed_result = format == ResponseFormat::ARROW
		                        ? TypedRowsFromArrowBody(context, db, response.body, max_bytes, deadline)
		                        : TypedRowsFromParquetBody(context, response.body, max_bytes, deadline);
		CheckTypedDecodeDeadline(context, deadline);
		response.typed_result = std::move(typed_result);
	} catch (const InterruptException &) {
		throw;
	} catch (const std::exception &e) {
		response.request_error = e.what();
	}
}

int64_t NonNegativeSetting(ClientContext &context, const string &name, int64_t fallback) {
	Value setting;
	if (!context.TryGetCurrentSetting(Identifier(name), setting) || setting.IsNull()) {
		return fallback;
	}
	const auto value = setting.GetValue<int64_t>();
	if (value < 0) {
		throw InvalidInputException("%s must be >= 0", name);
	}
	return value;
}

//! Admitted exec tasks across calls; OS worker counts belong to DuckDB.
std::atomic<int64_t> exec_tasks_running {0};

//! Reserve up to `wanted` exec tasks without passing `cap`. Never waits, so a full
//! process cannot leave an outer call waiting on inner calls that wait for a thread.
idx_t ReserveExecTasks(idx_t wanted, int64_t cap) {
	auto running = exec_tasks_running.load();
	for (;;) {
		const auto room = cap - running;
		if (room <= 0) {
			return 0;
		}
		const auto take = std::min<int64_t>(room, NumericCast<int64_t>(wanted));
		if (exec_tasks_running.compare_exchange_weak(running, running + take)) {
			return NumericCast<idx_t>(take);
		}
	}
}

//! One quackapi_selfdispatch call, retained by its native tasks, so a caller that
//! gives up (interrupt, deadline) returns while a row is still blocked in a host call.
struct ExecCall {
	shared_ptr<DatabaseInstance> db;
	vector<string> sqls;
	vector<bool> skip;
	vector<QuackapiHttpFetchResult> out;
	vector<bool> done;
	std::chrono::steady_clock::time_point deadline;
	idx_t max_response_bytes = 0;
	//! A served request's budgets, passed on so nested calls apply them too (0: none).
	int64_t serving_timeout_ms = 0;
	int64_t serving_max_response_bytes = 0;
	std::mutex mutex;
	std::condition_variable finished;
	unordered_set<Connection *> running;
	std::atomic<bool> cancelled {false};
	idx_t tasks_done = 0;
	std::atomic<idx_t> tasks_started {0};
	std::atomic<idx_t> cursor {0};
	unique_ptr<TaskExecutor> executor;
};

std::shared_ptr<ExecCall> NewExecCall(ClientContext &context, idx_t count) {
	auto call = std::make_shared<ExecCall>();
	call->db = context.db;
	call->sqls.resize(count);
	call->skip.assign(count, false);
	call->out.resize(count);
	call->done.assign(count, false);
	int64_t timeout_ms = 0;
	int64_t max_response_bytes = 0;
	if (QuackapiGetServingBudget(context, timeout_ms, max_response_bytes)) {
		call->serving_timeout_ms = timeout_ms;
		call->serving_max_response_bytes = max_response_bytes;
	} else {
		timeout_ms = PositiveSetting(context, "quackapi_query_timeout_ms", 30000);
		max_response_bytes = PositiveSetting(context, "quackapi_max_response_bytes", 16 * 1024 * 1024);
	}
	call->max_response_bytes = NumericCast<idx_t>(max_response_bytes);
	call->deadline = std::min(QuackapiContextDeadline(context),
	                          std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
	return call;
}

//! Keep interrupted operations alive until DuckDB's executor has retired their
//! tasks. Connection teardown drains the native executor before releasing the DB.
//! No worker is detached and no native task references a destroyed caller context.
struct ExecAbandonedRows : public ClientContextState {
	void Add(std::shared_ptr<ExecCall> call) {
		std::lock_guard<std::mutex> guard(mutex);
		for (auto it = calls.begin(); it != calls.end();) {
			if (it->use_count() == 1) {
				it = calls.erase(it);
			} else {
				++it;
			}
		}
		calls.push_back(std::move(call));
	}

	~ExecAbandonedRows() override {
		for (auto &call : calls) {
			if (call->executor) {
				call->executor->CancelAndDrain();
			}
		}
	}

	std::mutex mutex;
	vector<std::shared_ptr<ExecCall>> calls;
};

QuackapiHttpFetchResult ExecReceipt(QuackapiExecResponse response) {
	QuackapiHttpFetchResult receipt;
	receipt.status = static_cast<HTTPStatusCode>(response.status);
	receipt.reason = quackapi_httplib::status_message(response.status);
	receipt.body = std::move(response.body);
	receipt.typed_result = std::move(response.typed_result);
	for (auto &header : response.headers) {
		receipt.headers.Insert(header.first, header.second);
	}
	receipt.success = true;
	return receipt;
}

//! `parent` is set only for a row run on the calling thread, which the caller outlives.
void RunExecRow(ExecCall &call, idx_t i, ClientContext *parent) {
	QuackapiHttpFetchResult receipt;
	try {
		{
			std::lock_guard<std::mutex> guard(call.mutex);
			if (call.cancelled) {
				return;
			}
		}
		Connection con(*call.db);
		if (call.serving_timeout_ms > 0) {
			QuackapiSetServingBudget(con, call.serving_timeout_ms, call.serving_max_response_bytes);
		}
		struct Registration {
			ExecCall &call;
			Connection *con;
			~Registration() {
				std::lock_guard<std::mutex> guard(call.mutex);
				call.running.erase(con);
			}
		};
		{
			std::lock_guard<std::mutex> guard(call.mutex);
			if (call.cancelled) {
				return;
			}
			call.running.insert(&con);
		}
		Registration registration {call, &con};
		QuackapiQueryDeadline deadline(con, call.deadline, parent);
		auto cancelled = [&call, parent]() {
			return call.cancelled || (parent && parent->IsInterrupted());
		};
		receipt = ExecReceipt(
		    QuackapiExecSql(con, call.sqls[i], call.max_response_bytes, deadline, "body", "sql", cancelled, true));
	} catch (std::exception &e) {
		receipt.request_error = e.what();
	} catch (...) {
		receipt.request_error = "unknown error during quackapi_selfdispatch";
	}
	std::lock_guard<std::mutex> guard(call.mutex);
	call.out[i] = std::move(receipt);
	call.done[i] = true;
}

//! Stop waiting on `call`: no row or statement starts after this, and every running row
//! is interrupted.
void AbandonExecCall(ExecCall &call) {
	std::lock_guard<std::mutex> guard(call.mutex);
	call.cancelled = true;
	for (auto con : call.running) {
		con->Interrupt();
	}
}

thread_local bool in_native_exec_task = false;

struct NativeExecScope {
	bool previous = in_native_exec_task;
	NativeExecScope() {
		in_native_exec_task = true;
	}
	~NativeExecScope() {
		in_native_exec_task = previous;
	}
};

//! One resumable SQL row. Submit/Materialize do not run the query; each Step
//! participates in at most one engine task before yielding back to its scheduler.
class ExecOperation {
public:
	ExecOperation(ExecCall &call, idx_t row, ClientContext *parent = nullptr) : call(call), row(row), parent(parent) {
	}
	~ExecOperation() {
		if (connection) {
			std::lock_guard<std::mutex> guard(call.mutex);
			call.running.erase(connection.get());
		}
	}
	bool Step() {
		if (finished) {
			return true;
		}
		NativeExecScope scope;
		ScopedOutboundDeadline thread_deadline(call.deadline);
		try {
			if (call.cancelled || (parent && parent->IsInterrupted()) ||
			    std::chrono::steady_clock::now() >= call.deadline) {
				if (connection) {
					connection->Interrupt();
				}
				return Finish(QuackapiExecTimeoutResponse());
			}
			if (!connection) {
				connection = make_uniq<Connection>(*call.db);
				if (call.serving_timeout_ms > 0) {
					QuackapiSetServingBudget(*connection, call.serving_timeout_ms, call.serving_max_response_bytes);
				}
				// Resumable operations can migrate between scheduler threads.
				// Persist only the connection budget; scope TLS to each Step.
				deadline = make_uniq<QuackapiQueryDeadline>(*connection, call.deadline, parent, false);
				{
					std::lock_guard<std::mutex> guard(call.mutex);
					call.running.insert(connection.get());
				}
				if (call.sqls[row].find('\0') != string::npos) {
					return Finish(QuackapiExecErrorResponse("SQL text must not contain null bytes"));
				}
				statements = connection->ExtractStatements(call.sqls[row]);
				if (statements.empty()) {
					return Finish(QuackapiExecErrorResponse("No statement to execute"));
				}
			}
			if (!result) {
				result = connection->Submit(std::move(statements[statement]));
				if (!result->HasError()) {
					result->Materialize();
				}
			}
			auto state = result->Poll();
			if (!IsTerminal(state)) {
				state = result->ExecuteTask();
			}
			if (!IsTerminal(state)) {
				return false;
			}
			// Poll observes readiness; Complete settles the retained handle and its
			// transaction before the following statement can use a DDL/DML result.
			if (!result->HasError()) {
				result->Complete();
			}
			if (result->HasError()) {
				auto error = result->GetError();
				if (statements.size() > 1) {
					error = StringUtil::Format("Statement %llu of %llu failed: %s", statement + 1, statements.size(),
					                           error);
				}
				return Finish(deadline->Expired() ? QuackapiExecTimeoutResponse() : QuackapiExecErrorResponse(error));
			}
			if (++statement < statements.size()) {
				result.reset();
				return false;
			}
			auto response = QuackapiExecResult(*result, call.max_response_bytes, true);
			return Finish(deadline->Expired() ? QuackapiExecTimeoutResponse() : std::move(response));
		} catch (std::exception &ex) {
			return Finish(std::chrono::steady_clock::now() >= call.deadline
			                  ? QuackapiExecTimeoutResponse()
			                  : QuackapiExecErrorResponse(ErrorData(ex).Message()));
		}
	}

private:
	bool Finish(QuackapiExecResponse response) {
		std::lock_guard<std::mutex> guard(call.mutex);
		call.out[row] = ExecReceipt(std::move(response));
		call.done[row] = true;
		finished = true;
		return true;
	}
	ExecCall &call;
	idx_t row;
	ClientContext *parent;
	unique_ptr<Connection> connection;
	unique_ptr<QuackapiQueryDeadline> deadline;
	vector<unique_ptr<SQLStatement>> statements;
	unique_ptr<QueryResult> result;
	idx_t statement = 0;
	bool finished = false;
};

class ExecExecutorTask : public BaseExecutorTask {
public:
	ExecExecutorTask(TaskExecutor &executor, std::shared_ptr<ExecCall> call)
	    : BaseExecutorTask(executor), call(std::move(call)) {
	}
	~ExecExecutorTask() override {
		Settle();
	}
	TaskExecutionResult ExecuteTaskStep() override {
		if (!began) {
			began = true;
			call->tasks_started++;
		}
		if (call->cancelled) {
			operation.reset();
			Settle();
			return TaskExecutionResult::TASK_FINISHED;
		}
		if (!operation) {
			idx_t i;
			do {
				i = call->cursor.fetch_add(1);
				if (i >= call->sqls.size()) {
					Settle();
					return TaskExecutionResult::TASK_FINISHED;
				}
			} while (call->skip[i]);
			operation = make_uniq<ExecOperation>(*call, i);
		}
		if (operation->Step()) {
			operation.reset();
		}
		return TaskExecutionResult::TASK_NOT_FINISHED;
	}
	void Cancel() override {
		Settle();
	}
	string TaskType() const override {
		return "QuackapiExec";
	}

private:
	void Settle() {
		if (settled) {
			return;
		}
		settled = true;
		exec_tasks_running--;
		std::lock_guard<std::mutex> guard(call->mutex);
		call->tasks_done++;
		call->finished.notify_one();
	}
	std::shared_ptr<ExecCall> call;
	unique_ptr<ExecOperation> operation;
	bool settled = false;
	bool began = false;
};

//! Scalar compatibility adapter over resumable native query tasks. DuckDB owns
//! their workers. Nested scalar bridges participate inline rather than waiting
//! for capacity in their own pool; table forms yield the pipeline instead.
//! Interrupts stop admission and every running connection. The caller returns
//! promptly while its connection retains and drains any unfinished native tasks.
void ExecScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	const auto count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);

	auto call = NewExecCall(context, count);
	idx_t active = 0;
	for (idx_t i = 0; i < count; i++) {
		auto sql = args.data[0].GetValue(i);
		if (sql.IsNull()) {
			call->skip[i] = true;
			continue;
		}
		call->sqls[i] = sql.GetValue<string>();
		active++;
	}

	const auto max_threads = NonNegativeSetting(context, "quackapi_selfdispatch_max_threads", 256);

	// An exec route answers 403 while any policy is bound: its SQL cannot be rewritten.
	auto &quackapi_state = QuackapiState::Get(*context.db);
	if (!quackapi_state.SnapshotRowAccessBindings().empty() || !quackapi_state.SnapshotMaskingBindings().empty()) {
		for (idx_t i = 0; i < count; i++) {
			result.SetValue(i, call->skip[i] ? Value(FetchResultType())
			                                 : ToValue(ExecReceipt(QuackapiExecPolicyDeniedResponse())));
		}
		return;
	}

	const idx_t wanted = active < MAX_OUTBOUND_FANOUT ? active : MAX_OUTBOUND_FANOUT;
	// A nested scalar bridge participates inline rather than waiting for another
	// task in the same bounded async pool. Native table operators yield instead.
	auto &scheduler = TaskScheduler::GetScheduler(context);
	const idx_t reserved = wanted == 0 || in_native_exec_task || scheduler.NumberOfThreads() <= 1
	                           ? 0
	                           : ReserveExecTasks(wanted, max_threads);
	if (reserved) {
		try {
			call->executor = make_uniq<TaskExecutor>(scheduler, TaskSchedulerType::REGULAR);
			context.registered_state->GetOrCreate<ExecAbandonedRows>("quackapi_exec_abandoned_rows")->Add(call);
		} catch (...) {
			exec_tasks_running -= NumericCast<int64_t>(reserved);
			throw;
		}
	}
	auto abandon = [&]() {
		AbandonExecCall(*call);
	};
	idx_t started = 0;
	for (; started < reserved;) {
		unique_ptr<ExecExecutorTask> task;
		try {
			task = make_uniq<ExecExecutorTask>(*call->executor, call);
			started++;
			call->executor->ScheduleTask(std::move(task));
		} catch (std::exception &) {
			// Native submission retires a constructed task. Any admitted task can
			// drain the remaining rows; if none ran, the bridge finishes them below.
			exec_tasks_running -= NumericCast<int64_t>(reserved - started);
			break;
		}
	}

	if (started == 0) {
		for (idx_t i = 0; i < count; i++) {
			if (context.IsInterrupted()) {
				throw InterruptException();
			}
			if (!call->skip[i]) {
				RunExecRow(*call, i, &context);
			}
		}
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
	} else {
		std::unique_lock<std::mutex> guard(call->mutex);
		while (call->tasks_done < started) {
			if (context.IsInterrupted()) {
				guard.unlock();
				abandon();
				throw InterruptException();
			}
			const auto now = std::chrono::steady_clock::now();
			if (now >= call->deadline) {
				guard.unlock();
				abandon();
				break;
			}
			call->finished.wait_until(guard, std::min(call->deadline, now + std::chrono::milliseconds(10)));
			if (call->tasks_started == 0) {
				// Every CPU worker can be inside a scalar compatibility callback.
				// Participate in our producer rather than waiting for unavailable workers.
				guard.unlock();
				call->executor->WorkOnTasks();
				guard.lock();
			}
		}
		if (guard.owns_lock()) {
			guard.unlock();
		}
		if (!call->cancelled) {
			call->executor->WorkOnTasks();
			for (idx_t i = 0; i < count; i++) {
				if (!call->skip[i] && !call->done[i]) {
					RunExecRow(*call, i, &context);
				}
			}
		}
	}

	context.BlockingInterruptCheck();
	std::lock_guard<std::mutex> guard(call->mutex);
	for (idx_t i = 0; i < count; i++) {
		if (call->skip[i]) {
			result.SetValue(i, Value(FetchResultType()));
		} else if (call->done[i]) {
			result.SetValue(i, ToValue(call->out[i]));
		} else {
			result.SetValue(i, ToValue(ExecReceipt(QuackapiExecTimeoutResponse())));
		}
	}
}

struct PoolGlobalState : public GlobalTableFunctionState {
	vector<QuackapiHttpPoolStats> rows;
	idx_t offset = 0;
};

struct FetchTableBindData : public TableFunctionData {
	string url;
	string body;
	string content_type = "application/json";
	ResponseFormat response_format = ResponseFormat::RAW;
	int64_t timeout_ms = 0;
	unordered_map<string, string> headers;
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<FetchTableBindData>(*this);
	}
};

struct FetchTableGlobalState : public GlobalTableFunctionState {
	QuackapiHttpFetchResult receipt;
	std::function<QuackapiHttpFetchResult()> perform;
	std::function<bool(QuackapiHttpFetchResult &)> step;
	bool ready = false;
	bool finished = false;
};

void AsyncReceiptExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	context.BlockingInterruptCheck();
	auto &state = data_p.global_state->Cast<FetchTableGlobalState>();
	if (state.finished) {
		data_p.async_result = AsyncResultType::FINISHED;
		return;
	}
	// A resumed scan arrives only after the previously scheduled step finished.
	while (!state.ready) {
		vector<unique_ptr<AsyncTask>> tasks;
		tasks.push_back(make_uniq<CallbackAsyncTask>([&state]() {
			if (state.step) {
				state.ready = state.step(state.receipt);
			} else {
				state.receipt = state.perform();
				state.ready = true;
			}
		}));
		auto pending = AsyncResult::FromTasks(std::move(tasks), TaskSchedulerType::ASYNC);
		if (data_p.HandleBlocked(pending)) {
			return;
		}
	}
	context.BlockingInterruptCheck();
	auto value = ToValue(state.receipt);
	auto &children = StructValue::GetChildren(value);
	for (idx_t col = 0; col < children.size(); col++) {
		output.data[col].SetValue(0, children[col]);
	}
	output.SetChildCardinality(1);
	state.finished = true;
	data_p.async_result = AsyncResultType::HAVE_MORE_OUTPUT;
}

void ReceiptColumns(vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result_type = FetchResultType();
	for (auto &child : StructType::GetChildTypes(result_type)) {
		names.push_back(child.first);
		return_types.push_back(child.second);
	}
}

struct ExecTableBindData : public TableFunctionData {
	string sql;
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ExecTableBindData>(*this);
	}
};

unique_ptr<FunctionData> PoolBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                  vector<Identifier> &names) {
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
	output.SetChildCardinality(row);
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
	ScalarFunctionSet post_set("quackapi_post");
	ScalarFunction post2("quackapi_post", {LogicalType::VARCHAR, LogicalType::VARCHAR}, result_type, PostScalar);
	post2.SetVolatile();
	post2.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	post2.GetSignature().AddKeywordOnly("response_format", LogicalType::VARCHAR, Value("raw"));
	post2.GetSignature().AddKeywordOnly("timeout_ms", LogicalType::BIGINT, Value(LogicalType::BIGINT));
	ScalarFunction post2j("quackapi_post", {LogicalType::VARCHAR, LogicalType::JSON()}, result_type, PostScalar);
	post2j.SetVolatile();
	post2j.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	post2j.GetSignature().AddKeywordOnly("response_format", LogicalType::VARCHAR, Value("raw"));
	post2j.GetSignature().AddKeywordOnly("timeout_ms", LogicalType::BIGINT, Value(LogicalType::BIGINT));
	ScalarFunction post3("quackapi_post", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                     result_type, PostScalar);
	post3.SetVolatile();
	post3.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	post3.GetSignature().AddKeywordOnly("response_format", LogicalType::VARCHAR, Value("raw"));
	post3.GetSignature().AddKeywordOnly("timeout_ms", LogicalType::BIGINT, Value(LogicalType::BIGINT));
	ScalarFunction post4("quackapi_post",
	                     {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, header_type}, result_type,
	                     PostScalar);
	post4.SetVolatile();
	post4.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	post4.GetSignature().AddKeywordOnly("response_format", LogicalType::VARCHAR, Value("raw"));
	post4.GetSignature().AddKeywordOnly("timeout_ms", LogicalType::BIGINT, Value(LogicalType::BIGINT));
	post_set.AddFunction(post2);
	post_set.AddFunction(post2j);
	post_set.AddFunction(post3);
	post_set.AddFunction(post4);
	RegisterFallible(loader, post_set);

	// Table forms yield the SQL pipeline while DuckDB's async pool performs the
	// request. Scalar overloads keep their existing receipt and row-alignment contract.
	TableFunction post_tf(
	    "quackapi_post", {LogicalType::VARCHAR, LogicalType::VARCHAR}, AsyncReceiptExec,
	    [](ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
	       vector<Identifier> &names) -> unique_ptr<FunctionData> {
		    if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
			    throw InvalidInputException("quackapi_post table arguments must be non-NULL");
		    }
		    auto bind = make_uniq<FetchTableBindData>();
		    bind->url = input.inputs[0].GetValue<string>();
		    bind->body = input.inputs[1].GetValue<string>();
		    auto content_type = input.named_parameters.find("content_type");
		    if (content_type != input.named_parameters.end() && !content_type->second.IsNull()) {
			    bind->content_type = content_type->second.GetValue<string>();
		    }
		    auto headers = input.named_parameters.find("headers");
		    if (headers != input.named_parameters.end() && !headers->second.IsNull()) {
			    bind->headers = HeadersFromValue(headers->second);
		    }
		    auto response_format = input.named_parameters.find("response_format");
		    if (response_format != input.named_parameters.end() && !response_format->second.IsNull()) {
			    bind->response_format = ParseResponseFormat(response_format->second.GetValue<string>());
		    }
		    ApplyResponseAccept(bind->headers, bind->response_format);
		    auto timeout = input.named_parameters.find("timeout_ms");
		    if (timeout != input.named_parameters.end() && !timeout->second.IsNull()) {
			    bind->timeout_ms = timeout->second.GetValue<int64_t>();
			    if (bind->timeout_ms <= 0) {
				    throw InvalidInputException("timeout_ms must be > 0");
			    }
		    }
		    ReceiptColumns(types, names);
		    return std::move(bind);
	    },
	    [](ClientContext &context, TableFunctionInitInput &input) -> unique_ptr<GlobalTableFunctionState> {
		    auto &bind = input.bind_data->Cast<FetchTableBindData>();
		    auto state = make_uniq<FetchTableGlobalState>();
		    const auto deadline = OutboundDeadline(context, bind.timeout_ms);
		    auto db = context.db;
		    state->perform = [&bind, db, deadline, &context]() {
			    ScopedOutboundDeadline scoped(deadline);
			    ScopedOutboundContext context_scope(context);
			    auto response = QuackapiHttpFetch::Post(*db, bind.url, bind.body, bind.content_type, bind.headers);
			    DecodePostResponse(context, *db, response, bind.response_format, deadline);
			    return response;
		    };
		    return std::move(state);
	    });
	AddNamedParameter(post_tf, "headers", header_type);
	AddNamedParameter(post_tf, "content_type", LogicalType::VARCHAR);
	AddNamedParameter(post_tf, "response_format", LogicalType::VARCHAR);
	AddNamedParameter(post_tf, "timeout_ms", LogicalType::BIGINT);
	loader.RegisterFunction(post_tf);

	// Local execution preserves native result rows in addition to receipt metadata.
	ScalarFunction exec("quackapi_selfdispatch", {LogicalType::VARCHAR}, result_type, ExecScalar);
	exec.SetVolatile();
	RegisterFallible(loader, exec);

	TableFunction exec_tf(
	    "quackapi_selfdispatch", {LogicalType::VARCHAR}, AsyncReceiptExec,
	    [](ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
	       vector<Identifier> &names) -> unique_ptr<FunctionData> {
		    if (input.inputs[0].IsNull()) {
			    throw InvalidInputException("quackapi_selfdispatch table argument must be non-NULL");
		    }
		    auto bind = make_uniq<ExecTableBindData>();
		    bind->sql = input.inputs[0].GetValue<string>();
		    ReceiptColumns(types, names);
		    return std::move(bind);
	    },
	    [](ClientContext &context, TableFunctionInitInput &input) -> unique_ptr<GlobalTableFunctionState> {
		    auto &bind = input.bind_data->Cast<ExecTableBindData>();
		    auto state = make_uniq<FetchTableGlobalState>();
		    auto call = NewExecCall(context, 1);
		    call->sqls[0] = bind.sql;
		    auto &registry = QuackapiState::Get(*context.db);
		    const bool denied =
		        !registry.SnapshotRowAccessBindings().empty() || !registry.SnapshotMaskingBindings().empty();
		    auto operation = std::make_shared<ExecOperation>(*call, 0, &context);
		    state->step = [call, operation, denied](QuackapiHttpFetchResult &receipt) {
			    if (denied) {
				    receipt = ExecReceipt(QuackapiExecPolicyDeniedResponse());
				    return true;
			    }
			    if (!operation->Step()) {
				    return false;
			    }
			    receipt = std::move(call->out[0]);
			    return true;
		    };
		    return std::move(state);
	    });
	loader.RegisterFunction(exec_tf);

	loader.RegisterFunction(TableFunction("quackapi_http_pool", {}, PoolExec, PoolBind, PoolInit));
}

} // namespace duckdb
