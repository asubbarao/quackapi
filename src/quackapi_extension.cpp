#define DUCKDB_EXTENSION_MAIN

#include "quackapi_extension.hpp"

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
#include "quackapi_http_fetch.hpp"
#include "quackapi_queue.hpp"
#include "quackapi_policy.hpp"
#include "quackapi_server.hpp"
#include "quackapi_state.hpp"
#include "quackapi_stream.hpp"
#include "quackapi_table_api.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// quackapi_serve([port[, host]]) — start serving registered routes
//===--------------------------------------------------------------------===//

struct ServeBindData : public TableFunctionData {
	string host = "127.0.0.1";
	int32_t port = 8000;
	string static_dir;
	//! Empty (default) = CORS off. Pass '*' or a comma-separated origin list.
	string cors_origins;
	//! Empty = not provided by operator. Named param wins over SET quackapi_memory_limit.
	string memory_limit;
	//! Batteries defaults — all correct-by-default for servers; overridable.
	string log_level = "info";
	bool access_log = true;
	bool enable_logging = true;
	bool health_routes = true;
	string threads;
	bool preserve_insertion_order = false;
	bool enable_http_metadata_cache = true;
	int32_t worker_threads = static_cast<int32_t>(QUACKAPI_DEFAULT_WORKER_THREADS);
	int32_t keep_alive_max_count = static_cast<int32_t>(QUACKAPI_DEFAULT_KEEP_ALIVE_MAX);
	int32_t keep_alive_timeout_sec = static_cast<int32_t>(QUACKAPI_DEFAULT_KEEP_ALIVE_TIMEOUT_SEC);
	int32_t read_timeout_sec = static_cast<int32_t>(QUACKAPI_DEFAULT_IO_TIMEOUT_SEC);
	int32_t write_timeout_sec = static_cast<int32_t>(QUACKAPI_DEFAULT_IO_TIMEOUT_SEC);
	//! Response compression (Accept-Encoding). Default true.
	bool compression = true;
	//! Min body size in bytes before compression. Default 256.
	idx_t compression_min_bytes = 256;
	bool finished = false;
};

static unique_ptr<FunctionData> ServeBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ServeBindData>();
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
	// cors_origins named param wins; else fall back to SET quackapi_cors_origins.
	auto cors_entry = input.named_parameters.find("cors_origins");
	if (cors_entry != input.named_parameters.end()) {
		bind_data->cors_origins = cors_entry->second.GetValue<string>();
	} else {
		Value setting;
		if (context.TryGetCurrentSetting("quackapi_cors_origins", setting) && !setting.IsNull()) {
			bind_data->cors_origins = setting.GetValue<string>();
		}
	}
	// memory_limit named param wins; else fall back to SET quackapi_memory_limit.
	// Empty means "not provided" — ApplyQuackapiServerDefaults uses non-clobber logic.
	auto mem_entry = input.named_parameters.find("memory_limit");
	if (mem_entry != input.named_parameters.end()) {
		bind_data->memory_limit = mem_entry->second.GetValue<string>();
	} else {
		Value setting;
		if (context.TryGetCurrentSetting("quackapi_memory_limit", setting) && !setting.IsNull()) {
			bind_data->memory_limit = setting.GetValue<string>();
		}
	}
	// log_level named param wins; else SET quackapi_log_level.
	auto log_entry = input.named_parameters.find("log_level");
	if (log_entry != input.named_parameters.end()) {
		bind_data->log_level = log_entry->second.GetValue<string>();
	} else {
		Value setting;
		if (context.TryGetCurrentSetting("quackapi_log_level", setting) && !setting.IsNull()) {
			auto s = setting.GetValue<string>();
			if (!s.empty()) {
				bind_data->log_level = s;
			}
		}
	}
	auto access_entry = input.named_parameters.find("access_log");
	if (access_entry != input.named_parameters.end()) {
		bind_data->access_log = access_entry->second.GetValue<bool>();
	}
	auto enlog_entry = input.named_parameters.find("enable_logging");
	if (enlog_entry != input.named_parameters.end()) {
		bind_data->enable_logging = enlog_entry->second.GetValue<bool>();
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
	// compression named param wins; else SET quackapi_compression (default true).
	auto comp_entry = input.named_parameters.find("compression");
	if (comp_entry != input.named_parameters.end()) {
		bind_data->compression = comp_entry->second.GetValue<bool>();
	} else {
		Value setting;
		if (context.TryGetCurrentSetting("quackapi_compression", setting) && !setting.IsNull()) {
			bind_data->compression = setting.GetValue<bool>();
		}
	}
	// compression_min_bytes named param wins; else SET (default 256).
	auto min_entry = input.named_parameters.find("compression_min_bytes");
	if (min_entry != input.named_parameters.end()) {
		auto v = min_entry->second.GetValue<int64_t>();
		if (v < 0) {
			throw InvalidInputException("quackapi_serve: compression_min_bytes must be >= 0");
		}
		bind_data->compression_min_bytes = static_cast<idx_t>(v);
	} else {
		Value setting;
		if (context.TryGetCurrentSetting("quackapi_compression_min_bytes", setting) && !setting.IsNull()) {
			auto v = setting.GetValue<int64_t>();
			if (v < 0) {
				throw InvalidInputException("quackapi_serve: compression_min_bytes must be >= 0");
			}
			bind_data->compression_min_bytes = static_cast<idx_t>(v);
		}
	}
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("listen_url");
	return std::move(bind_data);
}

//! When the core quack extension is loaded, point its auth settings at
//! quackapi's bridge functions so REST CREATE AUTH policy and quack RPC share
//! one machinery (quack_server.cpp EvaluateAuthQuery + extension options
//! quack_authentication_function / quack_authorization_function).
static void ComposeQuackAuthSettings(ClientContext &context) {
	auto &db = *context.db;
	auto &config = DBConfig::GetConfig(db);
	Value existing;
	// Only set when the option exists (quack is loaded). Never invent settings.
	if (config.TryGetCurrentSetting("quack_authentication_function", existing)) {
		// Install our bridge if still at quack's default token checker, or
		// already pointing at us (idempotent re-serve). Leave custom user
		// callbacks alone.
		auto current = existing.IsNull() ? string() : existing.GetValue<string>();
		if (current.empty() || current == "quack_check_token" || current == "quackapi_authentication") {
			config.SetOptionByName("quack_authentication_function", Value("quackapi_authentication"));
		}
	}
	if (config.TryGetCurrentSetting("quack_authorization_function", existing)) {
		auto current = existing.IsNull() ? string() : existing.GetValue<string>();
		if (current.empty() || current == "quack_nop_authorization" || current == "quackapi_authorization") {
			config.SetOptionByName("quack_authorization_function", Value("quackapi_authorization"));
		}
	}
}

static void ServeExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ServeBindData>();
	if (bind_data.finished) {
		return;
	}
	// Compose with quack's auth settings when present (no-op if quack unloaded).
	ComposeQuackAuthSettings(context);

	// Batteries-included serve options (all ON / server-optimal by default).
	QuackapiServeOptions opts;
	opts.static_dir = bind_data.static_dir;
	opts.cors_origins = bind_data.cors_origins;
	opts.memory_limit = bind_data.memory_limit;
	opts.log_level = ParseQuackapiLogLevel(bind_data.log_level);
	opts.access_log = bind_data.access_log;
	opts.enable_logging = bind_data.enable_logging;
	opts.health_routes = bind_data.health_routes;
	opts.threads = bind_data.threads;
	opts.preserve_insertion_order = bind_data.preserve_insertion_order;
	opts.enable_http_metadata_cache = bind_data.enable_http_metadata_cache;
	opts.worker_threads = bind_data.worker_threads;
	opts.keep_alive_max_count = bind_data.keep_alive_max_count;
	opts.keep_alive_timeout_sec = bind_data.keep_alive_timeout_sec;
	opts.read_timeout_sec = bind_data.read_timeout_sec;
	opts.write_timeout_sec = bind_data.write_timeout_sec;

	// Apply DuckDB SETs / logging / resource guards (overridable, never unsafe).
	ApplyQuackapiServerDefaults(context, opts);
	// Compose request_id source: community tsid if LOADable, else core uuidv7.
	ProbeQuackapiRequestIdSource(*context.db, opts);
	// Auto-register /health + /healthz into the route registry (listed by routes()).
	RegisterQuackapiHealthRoutes(*context.db, opts);

	// REST listener — justified by absence of a quack path-registration hook
	// (see /tmp/quackapi_onquack/ARCHITECTURE.md). Lifecycle mirrors
	// HttpQuackServer / QuackStorageExtensionInfo::CreateServer.
	opts.compression = bind_data.compression;
	opts.compression_min_bytes = bind_data.compression_min_bytes;
	QuackapiState::Get(*context.db).StartServer(*context.db, bind_data.host, bind_data.port, opts);
	output.SetValue(0, 0, Value(StringUtil::Format("http://%s:%d", bind_data.host, bind_data.port)));
	output.SetCardinality(1);
	bind_data.finished = true;
}

//===--------------------------------------------------------------------===//
// quackapi_stop([port]) — stop one server or all
//===--------------------------------------------------------------------===//

struct StopBindData : public TableFunctionData {
	int32_t port = 0; // 0 = all
	bool finished = false;
};

static unique_ptr<FunctionData> StopBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
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
	output.SetCardinality(1);
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
                                           vector<string> &names) {
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
		row++;
		state.offset++;
	}
	output.SetCardinality(row);
}

//===--------------------------------------------------------------------===//
// quackapi_servers() — list running servers
//===--------------------------------------------------------------------===//

struct ServersBindData : public TableFunctionData {};

struct ServersGlobalState : public GlobalTableFunctionState {
	vector<std::pair<string, int>> servers;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> ServersBind(ClientContext &, TableFunctionBindInput &,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("host");
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("port");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("listen_url");
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
		output.SetValue(0, row, Value(server.first));
		output.SetValue(1, row, Value::INTEGER(server.second));
		output.SetValue(2, row, Value(StringUtil::Format("http://%s:%d", server.first, server.second)));
		row++;
		state.offset++;
	}
	output.SetCardinality(row);
}

//===--------------------------------------------------------------------===//
// quackapi_http_util_name() — active outbound HTTPUtil (no curl_httpfs dep)
//===--------------------------------------------------------------------===//
// When curl_httpfs is LOADed this is typically "MultiCurl". Same underlying
// DBConfig::GetHTTPUtil().GetName() that curl_httpfs_http_util_name() reads.

static void HttpUtilNameFunction(DataChunk &, ExpressionState &state, Vector &result) {
	auto &db = *state.GetContext().db;
	result.Reference(Value(QuackapiHttpFetch::ActiveHttpUtilName(db)));
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
	// Empty (default) = leave DuckDB memory_limit alone if already non-default;
	// otherwise apply the 256MB serve guard. Serve-time memory_limit := '…' wins.
	config.AddExtensionOption("quackapi_memory_limit",
	                          "Memory limit applied by quackapi_serve (e.g. '4GB', '512MB'). "
	                          "Empty (default): do not clobber a non-default DuckDB memory_limit; "
	                          "only apply the 256MB serve default when nothing was configured. "
	                          "Overridden by memory_limit named parameter.",
	                          LogicalType::VARCHAR, Value(""));
	// SET quackapi_log_level = 'info' | 'debug' | 'warn' | 'error' | 'silent'
	// Default info — informative access + DuckDB logs, not silent.
	config.AddExtensionOption("quackapi_log_level",
	                          "Log verbosity for quackapi_serve: silent|error|warn|info|debug. "
	                          "Default info. Overridden by log_level named parameter.",
	                          LogicalType::VARCHAR, Value("info"));
	// SET quackapi_compression = true|false — response compression (zstd/gzip).
	// Default true. Serve-time compression := false opts out.
	config.AddExtensionOption("quackapi_compression",
	                          "Enable Accept-Encoding response compression on quackapi_serve "
	                          "(zstd preferred, then gzip). Default true. Overridden by "
	                          "compression named parameter.",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	// SET quackapi_compression_min_bytes = N — skip compression under this size.
	config.AddExtensionOption("quackapi_compression_min_bytes",
	                          "Minimum response body size (bytes) before compression. "
	                          "Default 256. Overridden by compression_min_bytes named parameter.",
	                          LogicalType::BIGINT, Value::BIGINT(256));

	// quackapi_serve() / quackapi_serve(port) with batteries-included options.
	// All logging / health / server SETs ON by default; every knob overridable.
	// Also carries compression + compression_min_bytes.
	TableFunctionSet serve_set("quackapi_serve");
	TableFunction serve("quackapi_serve", {LogicalType::INTEGER}, ServeExec, ServeBind);
	serve.named_parameters["host"] = LogicalType::VARCHAR;
	serve.named_parameters["static_dir"] = LogicalType::VARCHAR;
	serve.named_parameters["cors_origins"] = LogicalType::VARCHAR;
	serve.named_parameters["memory_limit"] = LogicalType::VARCHAR;
	serve.named_parameters["log_level"] = LogicalType::VARCHAR;
	serve.named_parameters["access_log"] = LogicalType::BOOLEAN;
	serve.named_parameters["enable_logging"] = LogicalType::BOOLEAN;
	serve.named_parameters["health_routes"] = LogicalType::BOOLEAN;
	serve.named_parameters["threads"] = LogicalType::VARCHAR;
	serve.named_parameters["preserve_insertion_order"] = LogicalType::BOOLEAN;
	serve.named_parameters["enable_http_metadata_cache"] = LogicalType::BOOLEAN;
	serve.named_parameters["worker_threads"] = LogicalType::INTEGER;
	serve.named_parameters["keep_alive_max_count"] = LogicalType::INTEGER;
	serve.named_parameters["keep_alive_timeout_sec"] = LogicalType::INTEGER;
	serve.named_parameters["read_timeout_sec"] = LogicalType::INTEGER;
	serve.named_parameters["write_timeout_sec"] = LogicalType::INTEGER;
	serve.named_parameters["compression"] = LogicalType::BOOLEAN;
	serve.named_parameters["compression_min_bytes"] = LogicalType::BIGINT;
	serve_set.AddFunction(serve);
	serve.arguments.clear();
	serve_set.AddFunction(serve);
	loader.RegisterFunction(serve_set);

	// quackapi_stop() / quackapi_stop(port)
	TableFunctionSet stop_set("quackapi_stop");
	TableFunction stop("quackapi_stop", {LogicalType::INTEGER}, StopExec, StopBind);
	stop_set.AddFunction(stop);
	stop.arguments.clear();
	stop_set.AddFunction(stop);
	loader.RegisterFunction(stop_set);

	loader.RegisterFunction(TableFunction("quackapi_routes", {}, RoutesExec, RoutesBind, RoutesInit));
	loader.RegisterFunction(TableFunction("quackapi_servers", {}, ServersExec, ServersBind, ServersInit));
	loader.RegisterFunction(GetQuackapiGroupsFunction());

	// Auth inspection + API key management (secrets/hashes never exposed).
	loader.RegisterFunction(GetQuackapiAuthsFunction());
	loader.RegisterFunction(GetQuackapiAddApiKeyFunction());

	// Row-access + masking policy inspection (claims-keyed, not DB roles).
	loader.RegisterFunction(GetQuackapiPoliciesFunction());

	// quack auth bridge: quackapi_authentication / quackapi_authorization /
	// quackapi_verify_auth — same signatures as quack_check_token /
	// quack_nop_authorization so they can be SET as quack_*_function.
	RegisterQuackAuthBridgeFunctions(loader);

	// CREATE STREAM + quackapi_streams() (SSE push; WS deferred on httplib).
	RegisterQuackapiStreamFunctions(loader);

	// Outbound client diagnostic — works with Built-In, HTTPFS, MultiCurl, …
	// Does NOT auto-LOAD curl_httpfs; missing companion must not fail LOAD quackapi.
	loader.RegisterFunction(ScalarFunction("quackapi_http_util_name", {}, LogicalType::VARCHAR, HttpUtilNameFunction));

	// Durable broker-less job queue (CREATE QUEUE + enqueue/dequeue/ack/nack).
	// Backing store is the plain quackapi_jobs table; worker = compose cronjob.
	RegisterQuackapiQueueFunctions(loader);

	// CREATE ROUTE / GROUP / AUTH / QUEUE / POLICY / STREAM / API FOR TABLE —
	// all first-class nouns registered.
	ExtensionCallbackManager::Get(db).Register(RouteDdlParserExtension());
	ExtensionCallbackManager::Get(db).Register(GroupDdlParserExtension());
	ExtensionCallbackManager::Get(db).Register(AuthDdlParserExtension());
	ExtensionCallbackManager::Get(db).Register(TableApiDdlParserExtension());
	ExtensionCallbackManager::Get(db).Register(QueueDdlParserExtension());
	// CREATE ROW ACCESS / MASKING POLICY + ALTER TABLE policy bind
	ExtensionCallbackManager::Get(db).Register(PolicyDdlParserExtension());
	// CREATE STREAM (SSE)
	ExtensionCallbackManager::Get(db).Register(StreamDdlParserExtension());
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
