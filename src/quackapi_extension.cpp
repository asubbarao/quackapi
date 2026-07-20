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

//! Apply resource guards recommended for production serve (valsafe HARDENING P1-2).
//! Operators can raise memory_limit before/after serve if handlers need more.
static void ApplyServeResourceGuards(ClientContext &context) {
	try {
		Connection con(*context.db);
		// Cap memory so a single range()/hash join cannot OOM the host.
		// DuckDB 1.5.4 has no statement_timeout; memory_limit is the main lever.
		auto res = con.Query("SET memory_limit TO '256MB'");
		if (res->HasError()) {
			fprintf(stderr, "quackapi: could not SET memory_limit: %s\n", res->GetError().c_str());
		}
	} catch (std::exception &ex) {
		fprintf(stderr, "quackapi: resource guard failed: %s\n", ex.what());
	} catch (...) {
		fprintf(stderr, "quackapi: resource guard failed: unknown exception\n");
	}
}

static void ServeExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ServeBindData>();
	if (bind_data.finished) {
		return;
	}
	// Compose with quack's auth settings when present (no-op if quack unloaded).
	ComposeQuackAuthSettings(context);
	// P1-2: default memory ceiling for the serve lifetime (override with SET).
	ApplyServeResourceGuards(context);
	// REST listener — justified by absence of a quack path-registration hook
	// (see /tmp/quackapi_onquack/ARCHITECTURE.md). Lifecycle mirrors
	// HttpQuackServer / QuackStorageExtensionInfo::CreateServer.
	QuackapiServeOptions opts;
	opts.static_dir = bind_data.static_dir;
	opts.cors_origins = bind_data.cors_origins;
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

static unique_ptr<FunctionData> RoutesBind(ClientContext &, TableFunctionBindInput &,
                                           vector<LogicalType> &return_types, vector<string> &names) {
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

	// quackapi_serve() / quackapi_serve(port) with optional host + static_dir + cors_origins
	TableFunctionSet serve_set("quackapi_serve");
	TableFunction serve("quackapi_serve", {LogicalType::INTEGER}, ServeExec, ServeBind);
	serve.named_parameters["host"] = LogicalType::VARCHAR;
	serve.named_parameters["static_dir"] = LogicalType::VARCHAR;
	serve.named_parameters["cors_origins"] = LogicalType::VARCHAR;
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

	// Auth inspection + API key management (secrets/hashes never exposed).
	loader.RegisterFunction(GetQuackapiAuthsFunction());
	loader.RegisterFunction(GetQuackapiAddApiKeyFunction());

	// Row-access + masking policy inspection (claims-keyed, not DB roles).
	loader.RegisterFunction(GetQuackapiPoliciesFunction());

	// quack auth bridge: quackapi_authentication / quackapi_authorization /
	// quackapi_verify_auth — same signatures as quack_check_token /
	// quack_nop_authorization so they can be SET as quack_*_function.
	RegisterQuackAuthBridgeFunctions(loader);

	// Outbound client diagnostic — works with Built-In, HTTPFS, MultiCurl, …
	// Does NOT auto-LOAD curl_httpfs; missing companion must not fail LOAD quackapi.
	loader.RegisterFunction(ScalarFunction("quackapi_http_util_name", {}, LogicalType::VARCHAR, HttpUtilNameFunction));

	// Durable broker-less job queue (CREATE QUEUE + enqueue/dequeue/ack/nack).
	// Backing store is the plain quackapi_jobs table; worker = compose cronjob.
	RegisterQuackapiQueueFunctions(loader);

	// CREATE ROUTE / DROP ROUTE and CREATE AUTH / DROP AUTH / CREATE QUEUE syntax
	ExtensionCallbackManager::Get(db).Register(RouteDdlParserExtension());
	ExtensionCallbackManager::Get(db).Register(AuthDdlParserExtension());
	ExtensionCallbackManager::Get(db).Register(TableApiDdlParserExtension());
	ExtensionCallbackManager::Get(db).Register(QueueDdlParserExtension());
	// CREATE ROW ACCESS / MASKING POLICY + ALTER TABLE policy bind
	ExtensionCallbackManager::Get(db).Register(PolicyDdlParserExtension());
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
