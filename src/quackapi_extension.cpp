#define DUCKDB_EXTENSION_MAIN

#include "quackapi_extension.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackapi_ddl.hpp"
#include "quackapi_server.hpp"
#include "quackapi_state.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// quackapi_serve([port[, host]]) — start serving registered routes
//===--------------------------------------------------------------------===//

struct ServeBindData : public TableFunctionData {
	string host = "127.0.0.1";
	int32_t port = 8000;
	bool finished = false;
};

static unique_ptr<FunctionData> ServeBind(ClientContext &, TableFunctionBindInput &input,
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
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("listen_url");
	return std::move(bind_data);
}

static void ServeExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ServeBindData>();
	if (bind_data.finished) {
		return;
	}
	QuackapiState::Get(*context.db).StartServer(*context.db, bind_data.host, bind_data.port);
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

struct RoutesBindData : public TableFunctionData {
	bool finished = false;
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
	return make_uniq<RoutesBindData>();
}

static void RoutesExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<RoutesBindData>();
	if (bind_data.finished) {
		return;
	}
	auto routes = QuackapiState::Get(*context.db).SnapshotRoutes();
	idx_t row = 0;
	for (auto &route : routes) {
		output.SetValue(0, row, Value(route.name));
		output.SetValue(1, row, Value(route.method));
		output.SetValue(2, row, Value(route.pattern));
		output.SetValue(3, row, Value::INTEGER(route.status));
		output.SetValue(4, row, Value(route.handler_sql));
		row++;
	}
	output.SetCardinality(row);
	bind_data.finished = true;
}

//===--------------------------------------------------------------------===//
// quackapi_servers() — list running servers
//===--------------------------------------------------------------------===//

struct ServersBindData : public TableFunctionData {
	bool finished = false;
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

static void ServersExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ServersBindData>();
	if (bind_data.finished) {
		return;
	}
	auto servers = QuackapiState::Get(*context.db).ListServers();
	idx_t row = 0;
	for (auto &server : servers) {
		output.SetValue(0, row, Value(server.first));
		output.SetValue(1, row, Value::INTEGER(server.second));
		output.SetValue(2, row, Value(StringUtil::Format("http://%s:%d", server.first, server.second)));
		row++;
	}
	output.SetCardinality(row);
	bind_data.finished = true;
}

//===--------------------------------------------------------------------===//
// Load
//===--------------------------------------------------------------------===//

static void LoadInternal(ExtensionLoader &loader) {
	// quackapi_serve() / quackapi_serve(port) with optional host named param
	TableFunctionSet serve_set("quackapi_serve");
	TableFunction serve("quackapi_serve", {LogicalType::INTEGER}, ServeExec, ServeBind);
	serve.named_parameters["host"] = LogicalType::VARCHAR;
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

	loader.RegisterFunction(TableFunction("quackapi_routes", {}, RoutesExec, RoutesBind));
	loader.RegisterFunction(TableFunction("quackapi_servers", {}, ServersExec, ServersBind));
	// The DDL rewrite target must be resolvable by name at plan time
	loader.RegisterFunction(GetApplyRouteFunction());

	// CREATE ROUTE / DROP ROUTE syntax
	auto &db = loader.GetDatabaseInstance();
	db.config.parser_extensions.push_back(RouteDdlParserExtension());
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
