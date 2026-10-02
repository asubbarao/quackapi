#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "duckdb/common/helper.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unique_ptr.hpp"

namespace duckdb_httplib {
class Server;
struct Request;
struct Response;
} // namespace duckdb_httplib

namespace duckdb {

class ClientContext;
class DatabaseInstance;
class Value;

//! Max request body accepted by quackapi (8 MiB). Larger bodies get 413.
static constexpr size_t QUACKAPI_PAYLOAD_MAX_LENGTH = 8ull * 1024ull * 1024ull;

//! Default HTTP worker thread-pool size (httplib TaskQueue).
static constexpr size_t QUACKAPI_DEFAULT_WORKER_THREADS = 32;
//! Default keep-alive max requests per connection.
static constexpr size_t QUACKAPI_DEFAULT_KEEP_ALIVE_MAX = 128;
//! Default keep-alive idle timeout (seconds).
static constexpr time_t QUACKAPI_DEFAULT_KEEP_ALIVE_TIMEOUT_SEC = 10;
//! Default socket read/write timeout (seconds).
static constexpr time_t QUACKAPI_DEFAULT_IO_TIMEOUT_SEC = 30;

//! Access-log / server log verbosity. Default INFO is informative, not silent.
enum class QuackapiLogLevel : uint8_t {
	SILENT = 0,
	ERROR_LEVEL = 1,
	WARN = 2,
	INFO = 3,
	DEBUG_LEVEL = 4,
};

//! Response compression policy. AUTO negotiates zstd/gzip from Accept-Encoding.
enum class QuackapiCompressionMode : uint8_t { OFF = 0, GZIP = 1, ZSTD = 2, AUTO = 3 };

//! Serve options (static files, CORS, batteries-included server defaults,
//! response compression). Defaults are correct-by-default for a server
//! process: logging on, health routes on, throughput-oriented DuckDB SETs
//! applied at serve time, and Accept-Encoding compression on (zstd preferred,
//! then gzip). CORS stays off (browser cross-origin blocked) until the
//! operator opts in with cors_origins.
struct QuackapiServeOptions {
	string static_dir;
	//! Empty = CORS disabled. "*" = reflect any Origin (or * when no Origin).
	//! Otherwise a comma-separated allow-list of origins.
	string cors_origins;

	// --- Batteries: logging (ON by default) ---
	//! Access log + server log verbosity. Default INFO.
	QuackapiLogLevel log_level = QuackapiLogLevel::INFO;
	//! Requests at or above this duration are included at WARN level.
	int64_t slow_request_ms = 1000;
	//! Emit one structured access-log line per request (stderr, JSON). Default true.
	bool access_log = true;
	//! When non-empty, append access-log rows to this operator-created table instead
	//! of stderr. The writer falls back to stderr if the table cannot be used.
	string access_log_table;
	//! Header names permitted in the access-log map; empty avoids retaining headers.
	string log_headers;
	//! Preserve the raw query string in the access log after key-based redaction.
	bool log_query = false;
	//! Enable DuckDB built-in QueryLog at serve (CALL enable_logging). Default
	//! **false** when DuckDB logging was not already enabled — per-handler QueryLog
	//! to stdout destroys HTTP throughput. Preserve prior operator logging unless
	//! explicitly overridden; use access_log for ops.
	bool enable_logging = false;
	//! True when the operator supplied the enable_logging named parameter.
	bool enable_logging_explicit = false;
	//! Number of completed TCP requests retained for quackapi_requests().
	//! Zero disables the ring and its allocation.
	int64_t request_ring = 10000;

	// --- Batteries: health routes (ON by default) ---
	//! Auto-register GET /health + GET /healthz. Default true.
	bool health_routes = true;

	// --- Batteries: transport (overridable; sensible server defaults) ---
	//! httplib worker threads (max concurrent handlers). Default 32.
	int32_t worker_threads = static_cast<int32_t>(QUACKAPI_DEFAULT_WORKER_THREADS);
	//! Keep-alive max requests per connection. Default 128.
	int32_t keep_alive_max_count = static_cast<int32_t>(QUACKAPI_DEFAULT_KEEP_ALIVE_MAX);
	//! Keep-alive idle timeout seconds. Default 10.
	int32_t keep_alive_timeout_sec = static_cast<int32_t>(QUACKAPI_DEFAULT_KEEP_ALIVE_TIMEOUT_SEC);
	//! Socket read timeout seconds. Default 30.
	int32_t read_timeout_sec = static_cast<int32_t>(QUACKAPI_DEFAULT_IO_TIMEOUT_SEC);
	//! Socket write timeout seconds. Default 30.
	int32_t write_timeout_sec = static_cast<int32_t>(QUACKAPI_DEFAULT_IO_TIMEOUT_SEC);
	//! Query deadline is distinct from socket I/O timeouts. Always finite.
	int64_t query_timeout_ms = 30000;
	int64_t max_response_bytes = 16 * 1024 * 1024;
	int32_t max_pending_requests = 256;

	// --- Batteries: DuckDB SETs applied at serve (overridable) ---
	//! Empty = apply non-clobber memory guard (256MB when still at system default).
	string memory_limit;
	//! Empty = leave DuckDB threads at system default (all cores). Else e.g. "8".
	string threads;
	//! When true (default), SET preserve_insertion_order=false for throughput.
	bool preserve_insertion_order = false;
	//! When true (default), SET enable_http_metadata_cache=true for outbound HTTP.
	bool enable_http_metadata_cache = true;

	// --- Batteries: outbound HTTP client (curl_httpfs preferred) ---
	//! Preference: "auto" (default — prefer curl_httpfs, fall back to httplib with
	//! loud reason), "curl" (require curl_httpfs — fail serve if unavailable), or
	//! "httplib" (skip curl_httpfs). Named param / SET quackapi_http_client.
	//! Does NOT touch the inbound httplib SERVER — only the client used by
	//! httpfs / read_* over https.
	string http_client = "auto";
	//! Filled at serve after probe: "curl" or "httplib".
	string http_client_active;
	//! Why active is what it is. Empty when curl is active after a successful
	//! probe. "operator_forced" when operator chose httplib.
	//! "curl_httpfs_unavailable" when auto fell back (never silent).
	string http_client_reason;

	//! Request-id source for X-Request-ID. Always **uuidv7** (C++ core, no SQL)
	//! so the hot path never pays a SELECT per request. Probe may still record
	//! "tsid" only if an operator forces a future path; default is uuidv7.
	string request_id_source;

	// --- Compression (auto by default) ---
	//! AUTO honors Accept-Encoding; GZIP/ZSTD restrict the negotiated coding.
	QuackapiCompressionMode compression = QuackapiCompressionMode::AUTO;
	//! Bodies smaller than this many bytes are left uncompressed (default 1024).
	idx_t compression_min_bytes = 1024;

	// --- Native Postgres (optional; same shape as FastAPI+psycopg) ---
	//! When non-empty, simple routes execute via libpq (thread-local conn,
	//! fresh PQexecParams per request) instead of DuckDB ATTACH.
	//! Empty = DuckDB path only. Example: postgresql://user:pass@127.0.0.1:5432/db
	string pg_dsn;
};

//! One completed request, shared by the stderr and table access-log sinks.
struct QuackapiRequestRecord {
	string request_id;
	int64_t received_at_micros;
	string method;
	string path;
	string route_name;
	string route_path;
	int status;
	double duration_ms;
	double sql_prepare_ms = -1;
	double sql_execute_ms = -1;
	int64_t rows_out = -1;
	int64_t bytes_in;
	int64_t bytes_out;
	string client_ip;
	string user_agent;
	string http_version;
	string error_type;
	string error_message;
	string trace_id;
	string span_id;
	string parent_span_id;
	bool sampled = true;
	bool headers_logged = false;
	unordered_map<string, string> headers;
	bool query_logged = false;
	string query;
};

struct QuackapiTelemetryStatus {
	string sink;
	string target;
	idx_t queued = 0;
	idx_t exported_total = 0;
	idx_t dropped_total = 0;
	string last_error;
	int64_t last_export_age_ms = -1;
};

class QuackapiTelemetrySink {
public:
	virtual ~QuackapiTelemetrySink() = default;

	virtual void Enqueue(const QuackapiRequestRecord &record) = 0;
	virtual void Flush() = 0;
	virtual QuackapiTelemetryStatus Status() const = 0;
};

//! Convert the opt-in request fields to the access-log MAP/VARCHAR values.
Value QuackapiRequestHeadersValue(const QuackapiRequestRecord &entry);
Value QuackapiRequestQueryValue(const QuackapiRequestRecord &entry);

//! Fixed storage keeps request recording bounded while the mutex makes snapshots
//! safe without holding the request path behind a reader's work.
class QuackapiRequestRing {
public:
	explicit QuackapiRequestRing(size_t capacity) : records(capacity) {
	}

	void Push(const QuackapiRequestRecord &record) {
		std::lock_guard<std::mutex> lock(mutex);
		if (records.empty()) {
			return;
		}
		records[head] = record;
		head = (head + 1) % records.size();
		if (count < records.size()) {
			count++;
		}
	}

	std::vector<QuackapiRequestRecord> Snapshot() const {
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<QuackapiRequestRecord> result;
		result.reserve(count);
		if (records.empty()) {
			return result;
		}
		const auto first = count == records.size() ? head : 0;
		for (size_t i = 0; i < count; i++) {
			result.push_back(records[(first + i) % records.size()]);
		}
		return result;
	}

private:
	std::vector<QuackapiRequestRecord> records;
	mutable std::mutex mutex;
	size_t head = 0;
	size_t count = 0;
};

//! Parse log_level named param / setting. Accepts silent|error|warn|info|debug
//! (case-insensitive). Unknown → INFO.
QuackapiLogLevel ParseQuackapiLogLevel(const string &raw);

//! REST sidecar that dispatches requests to routes in QuackapiState.
//!
//! Why a sidecar (architecture C): the core quack HttpQuackServer hardcodes
//! only GET `/`, OPTIONS `/quack`, POST `/quack` (duckdb-quack
//! src/quack_http_server.cpp) and exposes no path-registration hook. Plain
//! curl REST therefore cannot ride quack's listener without an upstream change.
//!
//! Lifecycle, bind discipline, and stop semantics intentionally mirror
//! HttpQuackServer / QuackServer (StopAccepting vs Close, synchronous
//! bind_to_port, detached destroy) so this file would read as a natural
//! chapter of duckdb-quack if a route hook were ever added.
class QuackapiHttpServer {
public:
	//! opts.static_dir: optional directory of files for unrouted GETs.
	//! opts.cors_origins: empty (default) = CORS off; "*" or list enables CORS
	//! headers on responses and automatic OPTIONS preflight.
	//! When bind_and_listen is false, no TCP socket is opened — only in-process
	//! Dispatch (quackapi_request) is supported.
	QuackapiHttpServer(DatabaseInstance &db, const string &host, int port, const QuackapiServeOptions &opts,
	                   bool bind_and_listen = true);
	~QuackapiHttpServer();

	//! Close the listener socket only; safe from a request-handler thread.
	//! Mirrors QuackServer::StopAccepting (quack_server.hpp).
	void StopAccepting();
	//! Stop accepting AND join listener threads. Must not be called from a
	//! worker thread (httplib's listen teardown joins all workers).
	//! Mirrors QuackServer::Close.
	void Close();

	//! Same handler path as the TCP server — for quackapi_request() tests.
	void Dispatch(const duckdb_httplib::Request &req, duckdb_httplib::Response &res);

	const string &Host() const {
		return host;
	}
	int Port() const {
		return port;
	}
	const string &CorsOrigins() const {
		return cors_origins;
	}
	const QuackapiServeOptions &Options() const {
		return options;
	}
	//! Copy the retained records oldest-first while holding only the ring lock.
	std::vector<QuackapiRequestRecord> SnapshotRequests() const;
	//! Keep per-sink counters queryable without exposing sink ownership to the registry.
	std::vector<QuackapiTelemetryStatus> SnapshotTelemetryStatus() const;
	//! Compatibility counter used by /healthz.
	idx_t AccessLogOverflowCount() const;
	//! True while the TCP listener thread is alive (false after StopAccepting).
	bool IsRunning() const {
		return is_running.load();
	}
	//! Monotonic serve start (for /healthz uptime_sec).
	std::chrono::steady_clock::time_point StartedAt() const {
		return started_at;
	}

private:
	static void ListenThread(QuackapiHttpServer *server);
	void HandleRequest(const duckdb_httplib::Request &req, duckdb_httplib::Response &res);
	void ApplyCorsHeaders(const duckdb_httplib::Request &req, duckdb_httplib::Response &res);
	string NextRequestId(DatabaseInstance &db);
	void EmitAccessLog(const QuackapiRequestRecord &entry);
	void MaybeCompressResponse(const duckdb_httplib::Request &req, duckdb_httplib::Response &res);

	weak_ptr<DatabaseInstance> db_ptr;
	string host;
	int port;
	string cors_origins;
	QuackapiServeOptions options;
	std::chrono::steady_clock::time_point started_at;
	QuackapiCompressionMode compression = QuackapiCompressionMode::AUTO;
	idx_t compression_min_bytes = 1024;
	unique_ptr<duckdb_httplib::Server> server;
	std::vector<std::thread> listen_threads;
	std::atomic<bool> is_running {false};
	unique_ptr<QuackapiRequestRing> request_ring;
	std::vector<unique_ptr<QuackapiTelemetrySink>> telemetry_sinks;
};

//! In-process HTTP-shape invoke (no TCP). Builds a Request, runs Dispatch, returns
//! status + body + response headers. path may include ?query.
//! Optional body for POST/PUT/PATCH (Content-Type application/json when non-empty).
//! req_headers: optional request headers (e.g. X-Request-ID, Accept, Authorization).
//! headers_out: response header map (first value per name; case as httplib stores it).
//! Optional pg_dsn: when non-empty, in-process dispatch uses the native libpq
//! path (same as quackapi_serve(..., pg_dsn := ...)). Empty keeps DuckDB-only.
void QuackapiInProcessRequest(DatabaseInstance &db, const string &method, const string &path, const string &body,
                              int &status_out, string &body_out, string &content_type_out,
                              const unordered_map<string, string> *req_headers = nullptr,
                              unordered_map<string, string> *headers_out = nullptr, const string &pg_dsn = string(),
                              const QuackapiServeOptions *request_options = nullptr);

//! Apply batteries-included DuckDB SETs / logging at quackapi_serve() time.
//! Overridable via QuackapiServeOptions; never disables safety features.
//! Returns a human-readable summary of what was applied (for docs / debugging).
string ApplyQuackapiServerDefaults(ClientContext &context, QuackapiServeOptions &opts);

//! Auto-register GET /health + GET /healthz into the route registry (OR REPLACE
//! reserved names). When opts.health_routes is false, drops those reserved
//! routes so a prior serve(true) → stop → serve(false) does not leave them.
void RegisterQuackapiHealthRoutes(DatabaseInstance &db, const QuackapiServeOptions &opts);

//! Probe community tsid extension once; set opts.request_id_source to "tsid" or
//! "uuidv7". Compose-only (LOAD); never fails serve.
void ProbeQuackapiRequestIdSource(DatabaseInstance &db, QuackapiServeOptions &opts);

//! TCP connect probe: true when host:port accepts a connection (any HTTP
//! response, including 404). Used by quackapi_wait for readiness.
bool QuackapiPortIsAccepting(const string &host, int port, int connect_timeout_ms = 200);

} // namespace duckdb
