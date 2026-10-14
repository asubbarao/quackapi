#pragma once

#include <atomic>
#include <chrono>
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

//! Serve options for static files, CORS, HTTP limits, logging, and compression.
//! CORS stays off until the operator opts in with cors_origins.
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
	//! Emit one structured request event through DuckDB's native logger. Default true.
	bool access_log = true;
	//! Header names permitted in the structured access log; empty omits headers.
	string log_headers;
	//! Preserve the raw query string in the access log after key-based redaction.
	bool log_query = false;

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

	//! Request-id source for X-Request-ID. Always **uuidv7** (C++ core, no SQL)
	//! so the hot path never pays a SELECT per request. Probe may still record
	//! "tsid" only if an operator forces a future path; default is uuidv7.
	string request_id_source;

	// --- Compression (auto by default) ---
	//! AUTO honors Accept-Encoding; GZIP/ZSTD restrict the negotiated coding.
	QuackapiCompressionMode compression = QuackapiCompressionMode::AUTO;
	//! Bodies smaller than this many bytes are left uncompressed (default 1024).
	idx_t compression_min_bytes = 1024;
};

//! One completed request, emitted through DuckDB's native logger.
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
};

//! In-process HTTP-shape invoke (no TCP). Builds a Request, runs Dispatch, returns
//! status + body + response headers. path may include ?query.
//! Optional body for POST/PUT/PATCH (Content-Type application/json when non-empty).
//! req_headers: optional request headers (e.g. X-Request-ID, Accept, Authorization).
//! headers_out: response header map (first value per name; case as httplib stores it).
void QuackapiInProcessRequest(DatabaseInstance &db, const string &method, const string &path, const string &body,
                              int &status_out, string &body_out, string &content_type_out,
                              const unordered_map<string, string> *req_headers = nullptr,
                              unordered_map<string, string> *headers_out = nullptr,
                              const QuackapiServeOptions *request_options = nullptr);

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
