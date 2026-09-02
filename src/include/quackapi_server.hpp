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
	ERROR = 1,
	WARN = 2,
	INFO = 3,
	DEBUG = 4,
};

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
	//! Emit one structured access-log line per request (stderr, JSON). Default true.
	bool access_log = true;
	//! Enable DuckDB built-in QueryLog at serve (CALL enable_logging). Default
	//! **false** — per-handler QueryLog to stdout destroys HTTP throughput.
	//! Opt in with enable_logging:=true for debugging; use access_log for ops.
	bool enable_logging = false;

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

	// --- Compression (ON by default) ---
	//! When true (default), honor Accept-Encoding: prefer zstd, then gzip.
	bool compression = true;
	//! Bodies smaller than this many bytes are left uncompressed (default 256).
	idx_t compression_min_bytes = 256;

	// --- Native Postgres (optional; same shape as FastAPI+psycopg) ---
	//! When non-empty, simple routes execute via libpq (thread-local conn,
	//! fresh PQexecParams per request) instead of DuckDB ATTACH.
	//! Empty = DuckDB path only. Example: postgresql://user:pass@127.0.0.1:5432/db
	string pg_dsn;
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
	void EmitAccessLog(const duckdb_httplib::Request &req, const duckdb_httplib::Response &res,
	                   const string &request_id, double latency_ms);
	void MaybeCompressResponse(const duckdb_httplib::Request &req, duckdb_httplib::Response &res);

	weak_ptr<DatabaseInstance> db_ptr;
	string host;
	int port;
	string cors_origins;
	QuackapiServeOptions options;
	std::chrono::steady_clock::time_point started_at;
	bool compression = true;
	idx_t compression_min_bytes = 256;
	unique_ptr<duckdb_httplib::Server> server;
	std::vector<std::thread> listen_threads;
	std::atomic<bool> is_running {false};
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
                              unordered_map<string, string> *headers_out = nullptr, const string &pg_dsn = string());

//! Drop in-process rate-limit buckets for a route name (all client keys).
//! Called on CREATE OR REPLACE / DROP ROUTE so a new registration starts fresh.
void QuackapiClearRouteRateLimit(const string &route_name);

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
