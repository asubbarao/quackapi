#define DUCKDB_EXTENSION_MAIN

#include "quackapi_extension.hpp"
#include "quackapi_middleware.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include <type_traits>

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/main/settings.hpp"

#include "quackapi_auth.hpp"
#include "quackapi_ddl.hpp"
#include "quackapi_from_x.hpp"
#include "quackapi_graphql.hpp"
#include "quackapi_http_fetch.hpp"
#include "quackapi_queue.hpp"
#include "quackapi_policy.hpp"
#include "quackapi_server.hpp"
#include "quackapi_state.hpp"
#include "quackapi_stream.hpp"
#include "quackapi_table_api.hpp"
#include "quackapi_util.hpp"

namespace duckdb {

// Process-level stop request from SIGTERM (and SIGINT when we own the handler).
// DuckDB CLI also sets ClientContext::interrupted on SIGINT — both are honored.
static std::atomic<bool> quackapi_signal_stop {false};
#ifndef _WIN32
static std::atomic<bool> quackapi_signal_handlers_installed {false};
static void (*quackapi_prev_sigint)(int) = SIG_DFL;
static void (*quackapi_prev_sigterm)(int) = SIG_DFL;

static void QuackapiSignalHandler(int sig) {
	quackapi_signal_stop.store(true);
	if (sig == SIGINT && quackapi_prev_sigint && quackapi_prev_sigint != SIG_DFL && quackapi_prev_sigint != SIG_IGN) {
		quackapi_prev_sigint(sig);
	} else if (sig == SIGTERM && quackapi_prev_sigterm && quackapi_prev_sigterm != SIG_DFL &&
	           quackapi_prev_sigterm != SIG_IGN) {
		quackapi_prev_sigterm(sig);
	}
}

static void QuackapiEnsureSignalHandlers() {
	bool expected = false;
	if (!quackapi_signal_handlers_installed.compare_exchange_strong(expected, true)) {
		return;
	}
	quackapi_prev_sigint = std::signal(SIGINT, QuackapiSignalHandler);
	quackapi_prev_sigterm = std::signal(SIGTERM, QuackapiSignalHandler);
}
#endif

static bool QuackapiStopRequested(ClientContext &context) {
	return context.IsInterrupted() || quackapi_signal_stop.load();
}

//! Hold until the server on port is gone, or SIGINT/SIGTERM / query interrupt.
//! On signal/interrupt, stop the port (or all servers) so listen threads exit.
static void QuackapiBlockUntilStopped(ClientContext &context, int32_t port) {
#ifndef _WIN32
	QuackapiEnsureSignalHandlers();
#endif
	auto &state = QuackapiState::Get(*context.db);
	while (state.HasServerOnPort(port)) {
		if (QuackapiStopRequested(context)) {
			quackapi_signal_stop.store(false);
			if (port > 0) {
				state.StopServer(port);
			} else {
				state.StopAllServers();
			}
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	if (context.IsInterrupted()) {
		throw InterruptException();
	}
}

//===--------------------------------------------------------------------===//
// quackapi_serve([port[, host]]) — start serving registered routes
//===--------------------------------------------------------------------===//

struct ServeBindData : public TableFunctionData {
	QuackapiServeOptions limits;
	string host = "127.0.0.1";
	int32_t port = 8000;
	string static_dir;
	//! Empty (default) = CORS off. Pass '*' or a comma-separated origin list.
	string cors_origins;
	//! Empty = not provided by operator. Named param wins over SET quackapi_memory_limit.
	string memory_limit;
	//! Batteries defaults — all correct-by-default for servers; overridable.
	string log_level = "info";
	string access_log = "true";
	bool enable_logging = false;
	bool health_routes = true;
	string threads;
	bool preserve_insertion_order = false;
	bool preserve_insertion_order_explicit = false;
	bool enable_http_metadata_cache = true;
	int32_t worker_threads = static_cast<int32_t>(QUACKAPI_DEFAULT_WORKER_THREADS);
	int32_t keep_alive_max_count = static_cast<int32_t>(QUACKAPI_DEFAULT_KEEP_ALIVE_MAX);
	int32_t keep_alive_timeout_sec = static_cast<int32_t>(QUACKAPI_DEFAULT_KEEP_ALIVE_TIMEOUT_SEC);
	int32_t read_timeout_sec = static_cast<int32_t>(QUACKAPI_DEFAULT_IO_TIMEOUT_SEC);
	int32_t write_timeout_sec = static_cast<int32_t>(QUACKAPI_DEFAULT_IO_TIMEOUT_SEC);
	//! Response compression policy (auto|gzip|zstd|off). Default auto.
	string compression = "auto";
	//! Min body size in bytes before compression. Default 1024.
	idx_t compression_min_bytes = 1024;
	//! Outbound HTTP client preference: auto|curl|httplib (default auto).
	string http_client = "auto";
	//! When true, hold the query open after listen_url until stop / SIGINT / SIGTERM.
	//! Default false — serve still returns immediately (backward compatible).
	bool block = false;
	//! First Exec emitted listen_url; second Exec blocks when block=true.
	bool started = false;
	bool finished = false;
	int64_t request_ring = 10000;
};

static void ParseAccessLogString(const string &raw, bool &enabled, string &table) {
	auto value = StringUtil::Lower(raw);
	if (value == "true") {
		enabled = true;
		table.clear();
	} else if (value == "false") {
		enabled = false;
		table.clear();
	} else if (!raw.empty()) {
		enabled = true;
		table = raw;
	} else {
		throw InvalidInputException("quackapi_serve: access_log must be true, false, or a table name");
	}
}

static string AccessLogValueString(const Value &value) {
	if (value.IsNull()) {
		throw InvalidInputException("quackapi_serve: access_log must not be NULL");
	}
	if (value.type().id() == LogicalTypeId::VARCHAR) {
		return value.GetValue<string>();
	}
	return value.ToString();
}

//! Resolve a named option before its matching setting, keeping the option's native type.
template <class T>
static T BindOptionValue(ClientContext &context, TableFunctionBindInput &input, const string &name,
                         const string &setting_name, T fallback, bool empty_setting_is_default = false) {
	auto named = input.named_parameters.find(Identifier(name));
	if (named != input.named_parameters.end()) {
		if (named->second.IsNull()) {
			throw InvalidInputException("%s must not be NULL", name);
		}
		return named->second.GetValue<T>();
	}
	Value setting;
	if (!context.TryGetCurrentSetting(Identifier(setting_name), setting) || setting.IsNull()) {
		return fallback;
	}
	auto value = setting.GetValue<T>();
	if constexpr (std::is_same<T, string>::value) {
		if (empty_setting_is_default && value.empty()) {
			return fallback;
		}
	}
	return value;
}

static void BindResourceLimits(ClientContext &context, TableFunctionBindInput &input, QuackapiServeOptions &opts) {
	auto read = [&](const string &name, int64_t fallback, int64_t maximum) {
		int64_t value = BindOptionValue<int64_t>(context, input, name, "quackapi_" + name, fallback);
		if (value <= 0 || value > maximum) {
			throw InvalidInputException("%s must be between 1 and %lld", name, maximum);
		}
		return value;
	};
	opts.query_timeout_ms = read("query_timeout_ms", 30000, 86400000);
	opts.max_response_bytes = read("max_response_bytes", 16 * 1024 * 1024, 1024LL * 1024 * 1024);
	opts.max_request_bytes =
	    read("max_request_bytes", static_cast<int64_t>(QUACKAPI_PAYLOAD_MAX_LENGTH), QUACKAPI_MAX_PAYLOAD_MAX_LENGTH);
	opts.max_pending_requests = static_cast<int32_t>(read("max_pending_requests", 256, 100000));
	opts.slow_request_ms = read("slow_request_ms", 1000, 86400000);
}

static void BindAccessLogPrivacy(ClientContext &context, TableFunctionBindInput &input, QuackapiServeOptions &opts) {
	opts.log_headers = BindOptionValue<string>(context, input, "log_headers", "quackapi_log_headers", opts.log_headers);
	opts.log_query = BindOptionValue<bool>(context, input, "log_query", "quackapi_log_query", opts.log_query);
}

static unique_ptr<FunctionData> ServeBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto bind_data = make_uniq<ServeBindData>();
	BindResourceLimits(context, input, bind_data->limits);
	BindAccessLogPrivacy(context, input, bind_data->limits);
	if (!input.inputs.empty()) {
		bind_data->port = input.inputs[0].GetValue<int32_t>();
	}
	if (bind_data->port < 1 || bind_data->port > 65535) {
		throw InvalidInputException("quackapi_serve: port must be between 1 and 65535");
	}
	auto host_entry = input.named_parameters.find("host");
	if (host_entry != input.named_parameters.end()) {
		bind_data->host = host_entry->second.GetValue<string>();
	}
	auto static_entry = input.named_parameters.find("static_dir");
	if (static_entry != input.named_parameters.end()) {
		bind_data->static_dir = static_entry->second.GetValue<string>();
	}
	bind_data->cors_origins = BindOptionValue<string>(context, input, "cors_origins", "quackapi_cors_origins", "");
	// memory_limit named param wins; else fall back to SET quackapi_memory_limit.
	// Empty means "not provided" — ApplyQuackapiServerDefaults leaves memory_limit alone.
	bind_data->memory_limit = BindOptionValue<string>(context, input, "memory_limit", "quackapi_memory_limit", "");
	// log_level named param wins; else SET quackapi_log_level.
	bind_data->log_level = BindOptionValue<string>(context, input, "log_level", "quackapi_log_level", "info", true);
	// request_ring named param wins; else SET quackapi_request_ring.
	bind_data->request_ring = BindOptionValue<int64_t>(context, input, "request_ring", "quackapi_request_ring", 10000);
	// The ring is allocated up front at serve time, so bound it to keep a typo from exhausting memory.
	if (bind_data->request_ring < 0 || bind_data->request_ring > 1000000) {
		throw InvalidInputException("quackapi_serve: request_ring must be between 0 and 1000000");
	}
	auto access_entry = input.named_parameters.find("access_log");
	if (access_entry != input.named_parameters.end()) {
		bind_data->access_log = AccessLogValueString(access_entry->second);
		bool enabled;
		string table;
		ParseAccessLogString(bind_data->access_log, enabled, table);
	}
	auto enlog_entry = input.named_parameters.find("enable_logging");
	if (enlog_entry != input.named_parameters.end()) {
		bind_data->enable_logging = enlog_entry->second.GetValue<bool>();
		bind_data->limits.enable_logging_explicit = true;
	}
	auto health_entry = input.named_parameters.find("health_routes");
	if (health_entry != input.named_parameters.end()) {
		bind_data->health_routes = health_entry->second.GetValue<bool>();
	}
	auto threads_entry = input.named_parameters.find("threads");
	if (threads_entry != input.named_parameters.end()) {
		// Accept INTEGER or VARCHAR.
		if (threads_entry->second.type().id() == LogicalTypeId::VARCHAR) {
			bind_data->threads = threads_entry->second.GetValue<string>();
		} else {
			bind_data->threads = threads_entry->second.ToString();
		}
	}
	auto pio_entry = input.named_parameters.find("preserve_insertion_order");
	if (pio_entry != input.named_parameters.end()) {
		bind_data->preserve_insertion_order = pio_entry->second.GetValue<bool>();
		bind_data->preserve_insertion_order_explicit = true;
	}
	auto http_meta_entry = input.named_parameters.find("enable_http_metadata_cache");
	if (http_meta_entry != input.named_parameters.end()) {
		bind_data->enable_http_metadata_cache = http_meta_entry->second.GetValue<bool>();
	}
	auto wt_entry = input.named_parameters.find("worker_threads");
	if (wt_entry != input.named_parameters.end()) {
		bind_data->worker_threads = wt_entry->second.GetValue<int32_t>();
	}
	auto kam_entry = input.named_parameters.find("keep_alive_max_count");
	if (kam_entry != input.named_parameters.end()) {
		bind_data->keep_alive_max_count = kam_entry->second.GetValue<int32_t>();
	}
	auto kat_entry = input.named_parameters.find("keep_alive_timeout_sec");
	if (kat_entry != input.named_parameters.end()) {
		bind_data->keep_alive_timeout_sec = kat_entry->second.GetValue<int32_t>();
	}
	auto rt_entry = input.named_parameters.find("read_timeout_sec");
	if (rt_entry != input.named_parameters.end()) {
		bind_data->read_timeout_sec = rt_entry->second.GetValue<int32_t>();
	}
	auto wrt_entry = input.named_parameters.find("write_timeout_sec");
	if (wrt_entry != input.named_parameters.end()) {
		bind_data->write_timeout_sec = wrt_entry->second.GetValue<int32_t>();
	}
	auto comp_entry = input.named_parameters.find("compression");
	if (comp_entry != input.named_parameters.end()) {
		if (comp_entry->second.IsNull()) {
			throw InvalidInputException("compression must not be NULL");
		}
		if (comp_entry->second.type().id() == LogicalTypeId::BOOLEAN) {
			// Preserve the pre-enum boolean API: true means auto, false means off.
			bind_data->compression = comp_entry->second.GetValue<bool>() ? "auto" : "off";
		} else {
			bind_data->compression = comp_entry->second.GetValue<string>();
		}
	} else {
		bind_data->compression = BindOptionValue<string>(context, input, "compression", "quackapi_compression", "auto");
	}
	// compression_min_bytes named param wins; else SET (default 1024).
	// Keep legacy options recognizable, but never silently change their backend.
	string legacy_pg_dsn;
	auto pg_dsn_entry = input.named_parameters.find("pg_dsn");
	if (pg_dsn_entry != input.named_parameters.end() && !pg_dsn_entry->second.IsNull()) {
		legacy_pg_dsn = pg_dsn_entry->second.GetValue<string>();
	} else {
		Value setting;
		if (context.TryGetCurrentSetting("quackapi_pg_dsn", setting) && !setting.IsNull())
			legacy_pg_dsn = setting.GetValue<string>();
	}
	if (!legacy_pg_dsn.empty())
		throw InvalidInputException(
		    "pg_dsn was removed; use DuckDB ATTACH ... (TYPE postgres) and reference the attached catalog");
	auto min_entry = input.named_parameters.find("compression_min_bytes");
	if (min_entry != input.named_parameters.end()) {
		auto v = min_entry->second.GetValue<int64_t>();
		if (v < 0) {
			throw InvalidInputException("quackapi_serve: compression_min_bytes must be >= 0");
		}
		bind_data->compression_min_bytes = static_cast<idx_t>(v);
	} else {
		auto v =
		    BindOptionValue<int64_t>(context, input, "compression_min_bytes", "quackapi_compression_min_bytes", 1024);
		if (v < 0) {
			throw InvalidInputException("quackapi_serve: compression_min_bytes must be >= 0");
		}
		bind_data->compression_min_bytes = static_cast<idx_t>(v);
	}
	bind_data->http_client =
	    BindOptionValue<string>(context, input, "http_client", "quackapi_http_client", "auto", true);
	auto cancel_entry = input.named_parameters.find("cancel_on_disconnect");
	if (cancel_entry != input.named_parameters.end()) {
		if (cancel_entry->second.IsNull()) {
			throw InvalidInputException("cancel_on_disconnect must not be NULL");
		}
		bind_data->limits.cancel_on_disconnect = cancel_entry->second.GetValue<bool>();
	}
	auto block_entry = input.named_parameters.find("block");
	if (block_entry != input.named_parameters.end()) {
		bind_data->block = block_entry->second.GetValue<bool>();
	}
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("listen_url");
	return std::move(bind_data);
}

static void ServeExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ServeBindData>();
	if (bind_data.finished) {
		return;
	}
	// Phase 2: listen_url already emitted; hold until stop / signal.
	if (bind_data.started) {
		if (bind_data.block) {
			QuackapiBlockUntilStopped(context, bind_data.port);
		}
		bind_data.finished = true;
		return;
	}
	// Batteries-included serve options (all ON / server-optimal by default).
	QuackapiServeOptions opts = bind_data.limits;
	opts.static_dir = bind_data.static_dir;
	opts.cors_origins = bind_data.cors_origins;
	opts.memory_limit = bind_data.memory_limit;
	opts.log_level = ParseQuackapiLogLevel(bind_data.log_level);
	opts.request_ring = bind_data.request_ring;
	ParseAccessLogString(bind_data.access_log, opts.access_log, opts.access_log_table);
	opts.enable_logging = bind_data.enable_logging;
	opts.health_routes = bind_data.health_routes;
	opts.threads = bind_data.threads;
	opts.preserve_insertion_order = bind_data.preserve_insertion_order;
	opts.preserve_insertion_order_explicit = bind_data.preserve_insertion_order_explicit;
	opts.enable_http_metadata_cache = bind_data.enable_http_metadata_cache;
	opts.worker_threads = bind_data.worker_threads;
	opts.keep_alive_max_count = bind_data.keep_alive_max_count;
	opts.keep_alive_timeout_sec = bind_data.keep_alive_timeout_sec;
	opts.read_timeout_sec = bind_data.read_timeout_sec;
	opts.write_timeout_sec = bind_data.write_timeout_sec;
	opts.http_client = bind_data.http_client;

	// Apply DuckDB SETs / logging / resource guards (overridable, never unsafe).
	// Outbound client: auto prefers curl_httpfs (loud fallback); curl requires it.
	ApplyQuackapiServerDefaults(context, opts);
	// Compose request_id source: community tsid if LOADable, else core uuidv7.
	ProbeQuackapiRequestIdSource(*context.db, opts);
	// Auto-register /health + /healthz into the route registry (listed by routes()).
	RegisterQuackapiHealthRoutes(*context.db, opts);

	// REST listener — justified by absence of a quack path-registration hook
	// (see /tmp/quackapi_onquack/ARCHITECTURE.md). Lifecycle mirrors
	// HttpQuackServer / QuackStorageExtensionInfo::CreateServer.
	{
		auto compression = StringUtil::Lower(bind_data.compression);
		StringUtil::Trim(compression);
		if (compression == "off" || compression == "false") {
			opts.compression = QuackapiCompressionMode::OFF;
		} else if (compression == "gzip") {
			opts.compression = QuackapiCompressionMode::GZIP;
		} else if (compression == "zstd") {
			opts.compression = QuackapiCompressionMode::ZSTD;
		} else if (compression == "auto" || compression == "true") {
			opts.compression = QuackapiCompressionMode::AUTO;
		} else {
			throw InvalidInputException("quackapi_serve: compression must be one of off, gzip, zstd, auto");
		}
	}
	opts.compression_min_bytes = bind_data.compression_min_bytes;
	QuackapiState::Get(*context.db).StartServer(*context.db, bind_data.host, bind_data.port, opts);
	output.SetValue(0, 0, Value(StringUtil::Format("http://%s:%d", bind_data.host, bind_data.port)));
	output.SetChildCardinality(1);
	bind_data.started = true;
	// Emit listen_url now; block on the next Exec so the row reaches the client
	// before we hold the query open (supervised-process / launchd recipe).
	if (!bind_data.block) {
		bind_data.finished = true;
	}
}

//===--------------------------------------------------------------------===//
// quackapi_stop([port]) — stop one server or all
//===--------------------------------------------------------------------===//

struct StopBindData : public TableFunctionData {
	int32_t port = 0; // 0 = all
	bool finished = false;
};

static unique_ptr<FunctionData> StopBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto bind_data = make_uniq<StopBindData>();
	if (!input.inputs.empty()) {
		bind_data->port = input.inputs[0].GetValue<int32_t>();
	}
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("status");
	return std::move(bind_data);
}

static void StopExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<StopBindData>();
	if (bind_data.finished) {
		return;
	}
	auto &state = QuackapiState::Get(*context.db);
	string message;
	if (bind_data.port == 0) {
		state.StopAllServers();
		message = "Stopped all quackapi servers";
	} else if (state.StopServer(bind_data.port)) {
		message = StringUtil::Format("Stopped quackapi server on port %d", bind_data.port);
	} else {
		message = StringUtil::Format("No quackapi server on port %d", bind_data.port);
	}
	output.SetValue(0, 0, Value(message));
	output.SetChildCardinality(1);
	bind_data.finished = true;
}

//===--------------------------------------------------------------------===//
// quackapi_wait(port [, timeout_ms], host := …) — block until port accepts
// Replaces sleep/lsof/curl readiness loops. Returns one row: ready, listen_url.
//===--------------------------------------------------------------------===//

struct WaitBindData : public TableFunctionData {
	int32_t port = 0;
	//! Negative = wait forever (until accepting or interrupt).
	int64_t timeout_ms = -1;
	string host = "127.0.0.1";
	bool finished = false;
};

static unique_ptr<FunctionData> WaitBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.inputs.empty()) {
		throw InvalidInputException("quackapi_wait(port [, timeout_ms]) requires a port");
	}
	auto bind_data = make_uniq<WaitBindData>();
	bind_data->port = input.inputs[0].GetValue<int32_t>();
	if (bind_data->port < 1 || bind_data->port > 65535) {
		throw InvalidInputException("quackapi_wait: port must be between 1 and 65535");
	}
	if (input.inputs.size() >= 2 && !input.inputs[1].IsNull()) {
		bind_data->timeout_ms = input.inputs[1].GetValue<int64_t>();
	}
	auto host_entry = input.named_parameters.find("host");
	if (host_entry != input.named_parameters.end() && !host_entry->second.IsNull()) {
		bind_data->host = host_entry->second.GetValue<string>();
	}
	auto timeout_entry = input.named_parameters.find("timeout_ms");
	if (timeout_entry != input.named_parameters.end() && !timeout_entry->second.IsNull()) {
		bind_data->timeout_ms = timeout_entry->second.GetValue<int64_t>();
	}
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("ready");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("listen_url");
	return std::move(bind_data);
}

static void WaitExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<WaitBindData>();
	if (bind_data.finished) {
		return;
	}
#ifndef _WIN32
	QuackapiEnsureSignalHandlers();
#endif
	// Prefer the in-process host when this DB already started the server.
	string host = bind_data.host;
	string registered_host;
	if (QuackapiState::Get(*context.db).GetServerHost(bind_data.port, registered_host) && !registered_host.empty()) {
		host = registered_host;
	}

	const auto deadline = bind_data.timeout_ms < 0
	                          ? std::chrono::steady_clock::time_point::max()
	                          : std::chrono::steady_clock::now() + std::chrono::milliseconds(bind_data.timeout_ms);
	bool ready = false;
	while (true) {
		if (QuackapiStopRequested(context)) {
			quackapi_signal_stop.store(false);
			if (context.IsInterrupted()) {
				throw InterruptException();
			}
			break;
		}
		if (QuackapiPortIsAccepting(host, bind_data.port)) {
			ready = true;
			break;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}

	output.SetValue(0, 0, Value::BOOLEAN(ready));
	if (ready) {
		output.SetValue(1, 0, Value(StringUtil::Format("http://%s:%d", host, bind_data.port)));
	} else {
		output.SetValue(1, 0, Value());
	}
	output.SetChildCardinality(1);
	bind_data.finished = true;
}

//===--------------------------------------------------------------------===//
// quackapi_request(method, path [, body] [, headers := MAP]) — TestClient
// No TCP. Same handler path as serve. Community SQLLogic style.
//===--------------------------------------------------------------------===//

struct RequestBindData : public TableFunctionData {
	QuackapiServeOptions limits;
	string method;
	string path;
	string body;
	unordered_map<string, string> req_headers;
	//! Empty (default) keeps in-process requests quiet. Otherwise true, false, or a table name.
	string access_log;
	string log_level = "info";
	bool finished = false;
};

static unique_ptr<FunctionData> RequestBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.inputs.size() < 2 || input.inputs.size() > 3) {
		throw InvalidInputException(
		    "quackapi_request(method, path [, body] [, headers := MAP] [, pg_dsn := VARCHAR]) expects 2 or 3 "
		    "positional args");
	}
	auto bind_data = make_uniq<RequestBindData>();
	BindResourceLimits(context, input, bind_data->limits);
	BindAccessLogPrivacy(context, input, bind_data->limits);
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw InvalidInputException("quackapi_request: method and path must be non-NULL");
	}
	bind_data->method = input.inputs[0].ToString();
	bind_data->path = input.inputs[1].ToString();
	if (input.inputs.size() >= 3 && !input.inputs[2].IsNull()) {
		bind_data->body = input.inputs[2].ToString();
	}
	auto header_it = input.named_parameters.find("headers");
	if (header_it != input.named_parameters.end() && !header_it->second.IsNull()) {
		// MAP is LIST of STRUCT{key, value}
		for (auto &child : MapValue::GetChildren(header_it->second)) {
			auto &kv = StructValue::GetChildren(child);
			if (kv.size() >= 2 && !kv[0].IsNull() && !kv[1].IsNull()) {
				bind_data->req_headers[kv[0].ToString()] = kv[1].ToString();
			}
		}
	}
	string legacy_pg_dsn;
	auto pg_dsn_it = input.named_parameters.find("pg_dsn");
	if (pg_dsn_it != input.named_parameters.end() && !pg_dsn_it->second.IsNull()) {
		legacy_pg_dsn = pg_dsn_it->second.GetValue<string>();
	} else {
		Value setting;
		if (context.TryGetCurrentSetting("quackapi_pg_dsn", setting) && !setting.IsNull())
			legacy_pg_dsn = setting.GetValue<string>();
	}
	if (!legacy_pg_dsn.empty())
		throw InvalidInputException(
		    "pg_dsn was removed; use DuckDB ATTACH ... (TYPE postgres) and reference the attached catalog");
	auto access_entry = input.named_parameters.find("access_log");
	if (access_entry != input.named_parameters.end()) {
		bind_data->access_log = AccessLogValueString(access_entry->second);
		bool enabled;
		string table;
		ParseAccessLogString(bind_data->access_log, enabled, table);
	}
	bind_data->log_level = BindOptionValue<string>(context, input, "log_level", "quackapi_log_level", "info", true);
	// body is BLOB so parquet/arrow (and any non-UTF8) round-trip without
	// "Invalid unicode" on Value(string). JSON/text clients: decode(body).
	return_types.emplace_back(LogicalType::INTEGER);
	return_types.emplace_back(LogicalType::BLOB);
	return_types.emplace_back(LogicalType::VARCHAR);
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
	names.emplace_back("status");
	names.emplace_back("body");
	names.emplace_back("content_type");
	names.emplace_back("headers");
	return std::move(bind_data);
}

static void RequestExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<RequestBindData>();
	if (bind_data.finished) {
		return;
	}
	int status = 0;
	string body;
	string content_type;
	unordered_map<string, string> resp_headers;
	QuackapiServeOptions opts = bind_data.limits;
	opts.log_level = ParseQuackapiLogLevel(bind_data.log_level);
	opts.access_log = false;
	if (!bind_data.access_log.empty()) {
		ParseAccessLogString(bind_data.access_log, opts.access_log, opts.access_log_table);
	}
	QuackapiInProcessRequest(*context.db, bind_data.method, bind_data.path, bind_data.body, status, body, content_type,
	                         &bind_data.req_headers, &resp_headers, &opts);
	output.SetValue(0, 0, Value::INTEGER(status));
	output.SetValue(1, 0, Value::BLOB(const_data_ptr_cast(body.data()), body.size()));
	output.SetValue(2, 0, Value(content_type));
	vector<Value> hk;
	vector<Value> hv;
	hk.reserve(resp_headers.size());
	hv.reserve(resp_headers.size());
	for (auto &kv : resp_headers) {
		hk.emplace_back(kv.first);
		hv.emplace_back(kv.second);
	}
	output.SetValue(3, 0, Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, hk, hv));
	output.SetChildCardinality(1);
	bind_data.finished = true;
}

//===--------------------------------------------------------------------===//
// quackapi_routes() — inspect the route registry
//===--------------------------------------------------------------------===//

struct RoutesBindData : public TableFunctionData {};

struct RoutesGlobalState : public GlobalTableFunctionState {
	vector<QuackapiRoute> routes;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> RoutesBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                           vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("method");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("pattern");
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("status");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("handler");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("require_auth");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("group_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("tags");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("format");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("envelope");
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("empty_status");
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("timeout_sec");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("schedule");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("max_request_bytes");
	return make_uniq<RoutesBindData>();
}

static unique_ptr<GlobalTableFunctionState> RoutesInit(ClientContext &context, TableFunctionInitInput &) {
	auto state = make_uniq<RoutesGlobalState>();
	state->routes = QuackapiState::Get(*context.db).SnapshotRoutes();
	return std::move(state);
}

static void RoutesExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<RoutesGlobalState>();
	idx_t row = 0;
	while (state.offset < state.routes.size() && row < STANDARD_VECTOR_SIZE) {
		auto &route = state.routes[state.offset];
		output.SetValue(0, row, Value(route.name));
		output.SetValue(1, row, Value(route.method));
		output.SetValue(2, row, Value(route.pattern));
		output.SetValue(3, row, Value::INTEGER(route.status));
		output.SetValue(4, row, Value(route.handler_sql));
		output.SetValue(5, row, Value(route.require_auth));
		output.SetValue(6, row, Value(route.group_name));
		output.SetValue(7, row, Value(route.tags));
		output.SetValue(8, row, Value(route.response_format.empty() ? "json" : route.response_format));
		output.SetValue(9, row, Value(route.response_envelope.empty() ? "array" : route.response_envelope));
		output.SetValue(10, row, Value::INTEGER(route.empty_status));
		output.SetValue(11, row, Value::INTEGER(route.timeout_sec));
		output.SetValue(12, row, route.schedule.empty() ? Value() : Value(route.schedule));
		output.SetValue(13, row, route.max_request_bytes > 0 ? Value::BIGINT(route.max_request_bytes) : Value());
		row++;
		state.offset++;
	}
	output.SetChildCardinality(row);
}

//===--------------------------------------------------------------------===//
// quackapi_servers() — list running servers
//===--------------------------------------------------------------------===//

struct ServersBindData : public TableFunctionData {};

struct ServersGlobalState : public GlobalTableFunctionState {
	vector<std::tuple<string, int, string, string>> servers;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> ServersBind(ClientContext &, TableFunctionBindInput &,
                                            vector<LogicalType> &return_types, vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("host");
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("port");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("listen_url");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("http_client");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("http_client_reason");
	return make_uniq<ServersBindData>();
}

static unique_ptr<GlobalTableFunctionState> ServersInit(ClientContext &context, TableFunctionInitInput &) {
	auto state = make_uniq<ServersGlobalState>();
	state->servers = QuackapiState::Get(*context.db).ListServers();
	return std::move(state);
}

static void ServersExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<ServersGlobalState>();
	idx_t row = 0;
	while (state.offset < state.servers.size() && row < STANDARD_VECTOR_SIZE) {
		auto &server = state.servers[state.offset];
		const auto &host = std::get<0>(server);
		const auto port = std::get<1>(server);
		const auto &http_client = std::get<2>(server);
		const auto &http_client_reason = std::get<3>(server);
		output.SetValue(0, row, Value(host));
		output.SetValue(1, row, Value::INTEGER(port));
		output.SetValue(2, row, Value(StringUtil::Format("http://%s:%d", host, port)));
		output.SetValue(3, row, Value(http_client));
		output.SetValue(4, row, Value(http_client_reason));
		row++;
		state.offset++;
	}
	output.SetChildCardinality(row);
}

//===--------------------------------------------------------------------===//
// quackapi_requests([port]) — recent TCP requests from the in-memory ring
//===--------------------------------------------------------------------===//

struct RequestsBindData : public TableFunctionData {
	int32_t port = 0;
};

struct RequestsGlobalState : public GlobalTableFunctionState {
	vector<QuackapiRequestRecord> records;
	idx_t offset = 0;
};

static Value RequestsValue(const QuackapiRequestRecord &record, idx_t column) {
	switch (column) {
	case 0:
		return Value(record.request_id);
	case 1:
		return Value::TIMESTAMP(timestamp_t(record.received_at_micros));
	case 2:
		return Value(record.method);
	case 3:
		return Value(record.path);
	case 4:
		return record.route_name.empty() ? Value() : Value(record.route_name);
	case 5:
		return record.route_path.empty() ? Value() : Value(record.route_path);
	case 6:
		return Value::INTEGER(record.status);
	case 7:
		return Value::DOUBLE(record.duration_ms);
	case 8:
		return record.sql_prepare_ms < 0 ? Value() : Value::DOUBLE(record.sql_prepare_ms);
	case 9:
		return record.sql_execute_ms < 0 ? Value() : Value::DOUBLE(record.sql_execute_ms);
	case 10:
		return record.rows_out < 0 ? Value() : Value::BIGINT(record.rows_out);
	case 11:
		return Value::BIGINT(record.bytes_in);
	case 12:
		return Value::BIGINT(record.bytes_out);
	case 13:
		return Value(record.client_ip);
	case 14:
		return Value(record.user_agent);
	case 15:
		return record.http_version.empty() ? Value() : Value(record.http_version);
	case 16:
		return record.error_type.empty() ? Value() : Value(record.error_type);
	case 17:
		return record.error_message.empty() ? Value() : Value(record.error_message);
	case 18:
		return record.trace_id.empty() ? Value() : Value(record.trace_id);
	case 19:
		return record.span_id.empty() ? Value() : Value(record.span_id);
	case 20:
		return record.parent_span_id.empty() ? Value() : Value(record.parent_span_id);
	case 21:
		return Value::BOOLEAN(record.sampled);
	case 22:
		return QuackapiRequestHeadersValue(record);
	case 23:
		return QuackapiRequestQueryValue(record);
	default:
		throw InternalException("unknown quackapi request column");
	}
}

static unique_ptr<FunctionData> RequestsBind(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto bind_data = make_uniq<RequestsBindData>();
	const bool has_port = !input.inputs.empty() && !input.inputs[0].IsNull();
	if (has_port) {
		bind_data->port = input.inputs[0].GetValue<int32_t>();
	}
	if (has_port && (bind_data->port < 1 || bind_data->port > 65535)) {
		throw InvalidInputException("quackapi_requests: port must be between 1 and 65535");
	}
	const LogicalType types[] = {LogicalType::VARCHAR,
	                             LogicalType::TIMESTAMP,
	                             LogicalType::VARCHAR,
	                             LogicalType::VARCHAR,
	                             LogicalType::VARCHAR,
	                             LogicalType::VARCHAR,
	                             LogicalType::INTEGER,
	                             LogicalType::DOUBLE,
	                             LogicalType::DOUBLE,
	                             LogicalType::DOUBLE,
	                             LogicalType::BIGINT,
	                             LogicalType::BIGINT,
	                             LogicalType::BIGINT,
	                             LogicalType::VARCHAR,
	                             LogicalType::VARCHAR,
	                             LogicalType::VARCHAR,
	                             LogicalType::VARCHAR,
	                             LogicalType::VARCHAR,
	                             LogicalType::VARCHAR,
	                             LogicalType::VARCHAR,
	                             LogicalType::VARCHAR,
	                             LogicalType::BOOLEAN,
	                             LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
	                             LogicalType::VARCHAR};
	const char *const columns[] = {"request_id",     "received_at", "method",        "path",           "route_name",
	                               "route_path",     "status",      "duration_ms",   "sql_prepare_ms", "sql_execute_ms",
	                               "rows_out",       "bytes_in",    "bytes_out",     "client_ip",      "user_agent",
	                               "http_version",   "error_type",  "error_message", "trace_id",       "span_id",
	                               "parent_span_id", "sampled",     "headers",       "query"};
	for (idx_t i = 0; i < 24; i++) {
		return_types.emplace_back(types[i]);
		names.emplace_back(columns[i]);
	}
	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> RequestsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto state = make_uniq<RequestsGlobalState>();
	auto &bind_data = input.bind_data->Cast<RequestsBindData>();
	state->records = QuackapiState::Get(*context.db).SnapshotRequests(bind_data.port);
	return std::move(state);
}

static void RequestsExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<RequestsGlobalState>();
	idx_t row = 0;
	while (state.offset < state.records.size() && row < STANDARD_VECTOR_SIZE) {
		auto &record = state.records[state.offset];
		for (idx_t column = 0; column < 24; column++) {
			output.SetValue(column, row, RequestsValue(record, column));
		}
		row++;
		state.offset++;
	}
	output.SetChildCardinality(row);
}

//===--------------------------------------------------------------------===//
// Keep sink health queryable without making the writer implementation public.
//===--------------------------------------------------------------------===//

struct TelemetryStatusBindData : public TableFunctionData {
	int32_t port = 0;
};

struct TelemetryStatusGlobalState : public GlobalTableFunctionState {
	vector<QuackapiTelemetryStatus> statuses;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> TelemetryStatusBind(ClientContext &, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto bind_data = make_uniq<TelemetryStatusBindData>();
	const bool has_port = !input.inputs.empty() && !input.inputs[0].IsNull();
	if (has_port) {
		bind_data->port = input.inputs[0].GetValue<int32_t>();
	}
	if (has_port && (bind_data->port < 1 || bind_data->port > 65535)) {
		throw InvalidInputException("quackapi_telemetry_status: port must be between 1 and 65535");
	}
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("sink");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("target");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("queued");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("exported_total");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("dropped_total");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("last_error");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("last_export_age_ms");
	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> TelemetryStatusInit(ClientContext &context, TableFunctionInitInput &input) {
	auto state = make_uniq<TelemetryStatusGlobalState>();
	auto &bind_data = input.bind_data->Cast<TelemetryStatusBindData>();
	state->statuses = QuackapiState::Get(*context.db).SnapshotTelemetryStatus(bind_data.port);
	return std::move(state);
}

static void TelemetryStatusExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<TelemetryStatusGlobalState>();
	idx_t row = 0;
	while (state.offset < state.statuses.size() && row < STANDARD_VECTOR_SIZE) {
		auto &status = state.statuses[state.offset];
		output.SetValue(0, row, Value(status.sink));
		output.SetValue(1, row, Value(status.target));
		output.SetValue(2, row, Value::BIGINT(static_cast<int64_t>(status.queued)));
		output.SetValue(3, row, Value::BIGINT(static_cast<int64_t>(status.exported_total)));
		output.SetValue(4, row, Value::BIGINT(static_cast<int64_t>(status.dropped_total)));
		output.SetValue(5, row, Value(status.last_error));
		output.SetValue(6, row, Value::BIGINT(status.last_export_age_ms));
		row++;
		state.offset++;
	}
	output.SetChildCardinality(row);
}

struct TelemetryFlushBindData : public TableFunctionData {
	int32_t port = 0;
	bool finished = false;
};

static unique_ptr<FunctionData> TelemetryFlushBind(ClientContext &, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto bind_data = make_uniq<TelemetryFlushBindData>();
	const bool has_port = !input.inputs.empty() && !input.inputs[0].IsNull();
	if (has_port) {
		bind_data->port = input.inputs[0].GetValue<int32_t>();
	}
	if (has_port && (bind_data->port < 1 || bind_data->port > 65535)) {
		throw InvalidInputException("quackapi_telemetry_flush: port must be between 1 and 65535");
	}
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("status");
	return std::move(bind_data);
}

static void TelemetryFlushExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<TelemetryFlushBindData>();
	if (bind_data.finished) {
		return;
	}
	if (!QuackapiState::Get(*context.db).FlushTelemetry(bind_data.port)) {
		output.SetValue(0, 0, Value("No quackapi server to flush"));
	} else if (bind_data.port == 0) {
		output.SetValue(0, 0, Value("Flushed telemetry for all quackapi servers"));
	} else {
		output.SetValue(0, 0,
		                Value(StringUtil::Format("Flushed telemetry for quackapi server on port %d", bind_data.port)));
	}
	output.SetChildCardinality(1);
	bind_data.finished = true;
}

//===--------------------------------------------------------------------===//
// Load
//===--------------------------------------------------------------------===//

static void LoadInternal(ExtensionLoader &loader) {
	// SET quackapi_cors_origins = '*' | 'https://app.example,https://admin.example'
	// Default empty = CORS off. Serve-time cors_origins := '…' overrides this SET.
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	config.AddExtensionOption("quackapi_cors_origins",
	                          "CORS allowed origins for quackapi_serve (* or comma-separated list). "
	                          "Empty (default) disables CORS. Overridden by cors_origins named parameter.",
	                          LogicalType::VARCHAR, Value(""));
	// SET quackapi_memory_limit = '4GB' | '512MB' | …
	// Empty (default) = leave DuckDB memory_limit alone. Serve-time memory_limit := '…' wins.
	config.AddExtensionOption("quackapi_memory_limit",
	                          "Memory limit applied by quackapi_serve (e.g. '4GB', '512MB'). "
	                          "Empty (default): leave DuckDB memory_limit alone. "
	                          "Overridden by memory_limit named parameter.",
	                          LogicalType::VARCHAR, Value(""));
	// SET quackapi_log_level = 'info' | 'debug' | 'warn' | 'error' | 'silent'
	// Default info — informative access + DuckDB logs, not silent.
	config.AddExtensionOption("quackapi_log_level",
	                          "Log verbosity for quackapi_serve: silent|error|warn|info|debug. "
	                          "Default info. Overridden by log_level named parameter.",
	                          LogicalType::VARCHAR, Value("info"));
	config.AddExtensionOption("quackapi_slow_request_ms",
	                          "Requests at or above this duration are logged at warn level. "
	                          "Default 1000ms. Overridden by slow_request_ms named parameter.",
	                          LogicalType::BIGINT, Value::BIGINT(1000));
	config.AddExtensionOption("quackapi_request_ring",
	                          "Number of recent TCP requests retained by quackapi_requests(). "
	                          "Default 10000; zero disables the in-memory ring. Overridden by request_ring named "
	                          "parameter.",
	                          LogicalType::BIGINT, Value::BIGINT(10000));
	config.AddExtensionOption("quackapi_log_headers",
	                          "Comma-separated request-header allowlist for access-log maps; empty (default) "
	                          "avoids retaining headers. Overridden by log_headers named parameter.",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("quackapi_log_query",
	                          "Preserve a redacted raw query string in access logs; default false. Overridden by "
	                          "log_query named parameter.",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
	// SET quackapi_compression = 'auto'|'gzip'|'zstd'|'off'.
	// Default auto. Legacy true/false values map to auto/off.
	config.AddExtensionOption("quackapi_compression",
	                          "Response compression mode on quackapi_serve: auto, gzip, zstd or off. "
	                          "Default auto (zstd preferred, then gzip). Overridden by "
	                          "compression named parameter.",
	                          LogicalType::VARCHAR, Value("auto"));
	// SET quackapi_compression_min_bytes = N — skip compression under this size.
	config.AddExtensionOption("quackapi_compression_min_bytes",
	                          "Minimum response body size (bytes) before compression. "
	                          "Default 1024. Overridden by compression_min_bytes named parameter.",
	                          LogicalType::BIGINT, Value::BIGINT(1024));
	// SET quackapi_http_client = 'auto' | 'curl' | 'httplib'
	// Default auto: INSTALL+LOAD curl_httpfs; fall back to httplib with loud reason.
	// curl: REQUIRE curl_httpfs — fail serve if INSTALL/LOAD fails (no silent fallback).
	// httplib: force stock client. Serve-time http_client := '…' wins.
	// Inbound server remains httplib regardless.
	config.AddExtensionOption("quackapi_http_client",
	                          "Outbound HTTP client for httpfs/route fetches: auto|curl|httplib. "
	                          "Default auto prefers curl_httpfs (pool, HTTP/2, async) and falls back "
	                          "to httplib with http_client_reason on /healthz when unavailable. "
	                          "curl fails serve if curl_httpfs cannot INSTALL/LOAD. Overridden by "
	                          "http_client named parameter. Does not change the inbound HTTP server.",
	                          LogicalType::VARCHAR, Value("auto"));
	// Retain the legacy setting solely to produce an explicit migration error.
	config.AddExtensionOption("quackapi_pg_dsn", "Removed; attach PostgreSQL through DuckDB's postgres extension",
	                          LogicalType::VARCHAR, Value(""));

	config.AddExtensionOption("quackapi_query_timeout_ms", "Maximum query execution time in milliseconds",
	                          LogicalType::BIGINT, Value::BIGINT(30000));
	config.AddExtensionOption("quackapi_http_timeout_ms",
	                          "Maximum time for one outbound quackapi_post call in milliseconds", LogicalType::BIGINT,
	                          Value::BIGINT(600000));
	config.AddExtensionOption("quackapi_max_response_bytes", "Maximum uncompressed response bytes", LogicalType::BIGINT,
	                          Value::BIGINT(16 * 1024 * 1024));
	config.AddExtensionOption("quackapi_max_request_bytes", "Hard maximum HTTP request body bytes for quackapi_serve",
	                          LogicalType::BIGINT, Value::BIGINT(QUACKAPI_PAYLOAD_MAX_LENGTH));
	config.AddExtensionOption("quackapi_max_pending_requests", "Maximum queued HTTP connections", LogicalType::BIGINT,
	                          Value::BIGINT(256));
	config.AddExtensionOption("quackapi_exec_max_threads",
	                          "quackapi_exec tasks admitted to DuckDB's native scheduler at once; past it, rows run "
	                          "one at a time on the calling thread (0: always)",
	                          LogicalType::BIGINT, Value::BIGINT(256));
	config.AddExtensionOption("quackapi_graphql_allow_all", "Explicit legacy opt-in to public GraphQL catalog exposure",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
	// quackapi_serve() / quackapi_serve(port) with batteries-included options.
	// All logging / health / server SETs ON by default; every knob overridable.
	// Also carries compression + compression_min_bytes + http_client.
	TableFunctionSet serve_set("quackapi_serve");
	TableFunction serve("quackapi_serve", {LogicalType::INTEGER}, ServeExec, ServeBind);
	AddNamedParameter(serve, "host", LogicalType::VARCHAR);
	AddNamedParameter(serve, "static_dir", LogicalType::VARCHAR);
	AddNamedParameter(serve, "cors_origins", LogicalType::VARCHAR);
	AddNamedParameter(serve, "memory_limit", LogicalType::VARCHAR);
	AddNamedParameter(serve, "log_level", LogicalType::VARCHAR);
	AddNamedParameter(serve, "slow_request_ms", LogicalType::BIGINT);
	AddNamedParameter(serve, "request_ring", LogicalType::BIGINT);
	AddNamedParameter(serve, "log_headers", LogicalType::VARCHAR);
	AddNamedParameter(serve, "log_query", LogicalType::BOOLEAN);
	AddNamedParameter(serve, "access_log", LogicalType::ANY);
	AddNamedParameter(serve, "enable_logging", LogicalType::BOOLEAN);
	AddNamedParameter(serve, "health_routes", LogicalType::BOOLEAN);
	AddNamedParameter(serve, "threads", LogicalType::VARCHAR);
	AddNamedParameter(serve, "preserve_insertion_order", LogicalType::BOOLEAN);
	AddNamedParameter(serve, "enable_http_metadata_cache", LogicalType::BOOLEAN);
	AddNamedParameter(serve, "worker_threads", LogicalType::INTEGER);
	AddNamedParameter(serve, "keep_alive_max_count", LogicalType::INTEGER);
	AddNamedParameter(serve, "keep_alive_timeout_sec", LogicalType::INTEGER);
	AddNamedParameter(serve, "read_timeout_sec", LogicalType::INTEGER);
	AddNamedParameter(serve, "write_timeout_sec", LogicalType::INTEGER);
	AddNamedParameter(serve, "compression", LogicalType::ANY);
	AddNamedParameter(serve, "compression_min_bytes", LogicalType::BIGINT);
	AddNamedParameter(serve, "http_client", LogicalType::VARCHAR);
	AddNamedParameter(serve, "pg_dsn", LogicalType::VARCHAR);
	AddNamedParameter(serve, "block", LogicalType::BOOLEAN);
	AddNamedParameter(serve, "cancel_on_disconnect", LogicalType::BOOLEAN);
	AddNamedParameter(serve, "query_timeout_ms", LogicalType::BIGINT);
	AddNamedParameter(serve, "max_response_bytes", LogicalType::BIGINT);
	AddNamedParameter(serve, "max_request_bytes", LogicalType::BIGINT);
	AddNamedParameter(serve, "max_pending_requests", LogicalType::BIGINT);
	serve_set.AddFunction(serve);
	serve_set.AddFunction(WithoutPositionalArguments(serve));
	loader.RegisterFunction(serve_set);

	// quackapi_stop() / quackapi_stop(port)
	TableFunctionSet stop_set("quackapi_stop");
	TableFunction stop("quackapi_stop", {LogicalType::INTEGER}, StopExec, StopBind);
	stop_set.AddFunction(stop);
	stop_set.AddFunction(WithoutPositionalArguments(stop));
	loader.RegisterFunction(stop_set);

	// quackapi_wait(port) / quackapi_wait(port, timeout_ms) — readiness probe
	TableFunctionSet wait_set("quackapi_wait");
	TableFunction wait1("quackapi_wait", {LogicalType::INTEGER}, WaitExec, WaitBind);
	AddNamedParameter(wait1, "host", LogicalType::VARCHAR);
	AddNamedParameter(wait1, "timeout_ms", LogicalType::BIGINT);
	wait_set.AddFunction(wait1);
	wait_set.AddFunction(WithPositionalArguments(wait1, {LogicalType::INTEGER, LogicalType::BIGINT}));
	loader.RegisterFunction(wait_set);

	// quackapi_request — SQLLogic TestClient (no TCP, no .sh).
	TableFunctionSet request_set("quackapi_request");
	TableFunction request2("quackapi_request", {LogicalType::VARCHAR, LogicalType::VARCHAR}, RequestExec, RequestBind);
	AddNamedParameter(request2, "headers", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
	AddNamedParameter(request2, "access_log", LogicalType::ANY);
	AddNamedParameter(request2, "log_level", LogicalType::VARCHAR);
	AddNamedParameter(request2, "slow_request_ms", LogicalType::BIGINT);
	AddNamedParameter(request2, "log_headers", LogicalType::VARCHAR);
	AddNamedParameter(request2, "log_query", LogicalType::BOOLEAN);
	AddNamedParameter(request2, "pg_dsn", LogicalType::VARCHAR);
	AddNamedParameter(request2, "query_timeout_ms", LogicalType::BIGINT);
	AddNamedParameter(request2, "max_response_bytes", LogicalType::BIGINT);
	request_set.AddFunction(request2);
	request_set.AddFunction(
	    WithPositionalArguments(request2, {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}));
	loader.RegisterFunction(request_set);

	loader.RegisterFunction(TableFunction("quackapi_routes", {}, RoutesExec, RoutesBind, RoutesInit));
	loader.RegisterFunction(TableFunction("quackapi_servers", {}, ServersExec, ServersBind, ServersInit));
	TableFunctionSet requests_set("quackapi_requests");
	TableFunction requests1("quackapi_requests", {LogicalType::INTEGER}, RequestsExec, RequestsBind, RequestsInit);
	requests_set.AddFunction(requests1);
	requests_set.AddFunction(WithoutPositionalArguments(requests1));
	loader.RegisterFunction(requests_set);
	TableFunctionSet telemetry_status_set("quackapi_telemetry_status");
	TableFunction telemetry_status1("quackapi_telemetry_status", {LogicalType::INTEGER}, TelemetryStatusExec,
	                                TelemetryStatusBind, TelemetryStatusInit);
	telemetry_status_set.AddFunction(telemetry_status1);
	telemetry_status_set.AddFunction(WithoutPositionalArguments(telemetry_status1));
	loader.RegisterFunction(telemetry_status_set);
	TableFunctionSet telemetry_flush_set("quackapi_telemetry_flush");
	TableFunction telemetry_flush1("quackapi_telemetry_flush", {LogicalType::INTEGER}, TelemetryFlushExec,
	                               TelemetryFlushBind);
	telemetry_flush_set.AddFunction(telemetry_flush1);
	telemetry_flush_set.AddFunction(WithoutPositionalArguments(telemetry_flush1));
	loader.RegisterFunction(telemetry_flush_set);
	loader.RegisterFunction(GetQuackapiGroupsFunction());

	// Auth inspection + API key management (secrets/hashes never exposed).
	loader.RegisterFunction(GetQuackapiAuthsFunction());
	loader.RegisterFunction(GetQuackapiAddApiKeyFunction());

	// Row-access + masking policy inspection (claims-keyed, not DB roles).
	loader.RegisterFunction(GetQuackapiPoliciesFunction());

	// REST route auth verifier (independent of core quack RPC callbacks).
	RegisterQuackapiVerifyAuthFunction(loader);

	// CREATE STREAM + quackapi_streams() (SSE and WebSocket push).
	RegisterQuackapiStreamFunctions(loader);

	// Outbound HTTP with a connection pool that survives ACROSS requests:
	// quackapi_post / quackapi_http_pool. See the header of
	// quackapi_http_fetch.hpp for why the un-pooled path costs ~27ms a call.
	RegisterQuackapiHttpFetchFunctions(loader);

	// Durable broker-less job queue (CREATE QUEUE + enqueue/dequeue/ack/nack).
	// Backing store is the plain quackapi_jobs table; worker = compose cronjob.
	RegisterQuackapiQueueFunctions(loader);
	loader.RegisterFunction(GetApplyMiddlewareFunction());
	loader.RegisterFunction(GetQuackapiMiddlewaresFunction());

	// quack_from_{fastapi,rails,express,gin}[+_models]: sitting_duck extractors
	// that turn a real web-app source tree into route/model IR rows.
	RegisterQuackapiFromXFunctions(loader);

	// Thin GraphQL allowlist + named route inspection (CREATE GRAPHQL … DDL).
	RegisterQuackapiGraphqlFunctions(loader);

	// CREATE ROUTE / GROUP / AUTH / QUEUE / POLICY / STREAM / API FOR TABLE /
	// GRAPHQL FOR TABLE / GRAPHQL ROUTE — all first-class nouns registered.
	ExtensionCallbackManager::Get(db).Register(RouteDdlParserExtension());
	ExtensionCallbackManager::Get(db).Register(GroupDdlParserExtension());
	ExtensionCallbackManager::Get(db).Register(AuthDdlParserExtension());
	ExtensionCallbackManager::Get(db).Register(TableApiDdlParserExtension());
	ExtensionCallbackManager::Get(db).Register(QueueDdlParserExtension());
	ExtensionCallbackManager::Get(db).Register(MiddlewareDdlParserExtension());
	// CREATE ROW ACCESS / MASKING POLICY + ALTER TABLE policy bind
	ExtensionCallbackManager::Get(db).Register(PolicyDdlParserExtension());
	// CREATE STREAM (SSE)
	ExtensionCallbackManager::Get(db).Register(StreamDdlParserExtension());
	// CREATE / DROP GRAPHQL FOR TABLE (+ DROP GRAPHQL ALL) + GRAPHQL ROUTE
	ExtensionCallbackManager::Get(db).Register(GraphqlDdlParserExtension());
}

void QuackapiExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string QuackapiExtension::Name() {
	return "quackapi";
}

std::string QuackapiExtension::Version() const {
#ifdef EXT_VERSION_QUACKAPI
	return EXT_VERSION_QUACKAPI;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(quackapi, loader) {
	duckdb::LoadInternal(loader);
}
}
