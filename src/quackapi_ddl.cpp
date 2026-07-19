#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/parser/parser_extension.hpp"

#include "quackapi_ddl.hpp"
#include "quackapi_state.hpp"

namespace duckdb {

namespace {

string Trim(const string &input) {
	idx_t begin = 0;
	idx_t end = input.size();
	while (begin < end && StringUtil::CharacterIsSpace(input[begin])) {
		begin++;
	}
	while (end > begin && (StringUtil::CharacterIsSpace(input[end - 1]) || input[end - 1] == ';')) {
		end--;
	}
	return input.substr(begin, end - begin);
}

//! Parsed CREATE/DROP ROUTE statement, carried from parse to plan.
struct RouteDdlParseData : public ParserExtensionParseData {
	string action; // CREATE / DROP
	bool or_replace = false;
	QuackapiRoute route;

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto copy = make_uniq<RouteDdlParseData>();
		copy->action = action;
		copy->or_replace = or_replace;
		copy->route = route;
		return std::move(copy);
	}
	string ToString() const override {
		return action + " ROUTE " + route.name;
	}
};

bool IsHttpMethod(const string &method) {
	return method == "GET" || method == "POST" || method == "PUT" || method == "DELETE" || method == "PATCH" ||
	       method == "HEAD";
}

//! Grammar:
//!   CREATE [OR REPLACE] ROUTE <name> <METHOD> '<pattern>' [STATUS <n>] AS <select>
//!   DROP ROUTE <name>
ParserExtensionParseResult RouteDdlParse(ParserExtensionInfo *, const string &query) {
	auto q = Trim(query);
	auto upper = StringUtil::Upper(q);

	bool or_replace = false;
	idx_t pos;
	if (StringUtil::StartsWith(upper, "CREATE ROUTE ")) {
		pos = 13;
	} else if (StringUtil::StartsWith(upper, "CREATE OR REPLACE ROUTE ")) {
		pos = 24;
		or_replace = true;
	} else if (StringUtil::StartsWith(upper, "DROP ROUTE ")) {
		auto name = Trim(q.substr(11));
		if (name.empty() || name.find(' ') != string::npos) {
			return ParserExtensionParseResult("DROP ROUTE expects a single route name");
		}
		auto data = make_uniq<RouteDdlParseData>();
		data->action = "DROP";
		data->route.name = name;
		return ParserExtensionParseResult(std::move(data));
	} else {
		// Not ours — let the core parser produce its own error.
		return ParserExtensionParseResult();
	}

	auto rest = Trim(q.substr(pos));

	// <name> <METHOD>
	auto first_space = rest.find(' ');
	if (first_space == string::npos) {
		return ParserExtensionParseResult("CREATE ROUTE <name> <METHOD> '<pattern>' AS <select>");
	}
	auto name = rest.substr(0, first_space);
	rest = Trim(rest.substr(first_space));
	auto second_space = rest.find(' ');
	if (second_space == string::npos) {
		return ParserExtensionParseResult("Expected <METHOD> '<pattern>' after route name");
	}
	auto method = StringUtil::Upper(rest.substr(0, second_space));
	if (!IsHttpMethod(method)) {
		return ParserExtensionParseResult("Unknown HTTP method \"" + method +
		                                  "\" — expected GET, POST, PUT, DELETE, PATCH or HEAD");
	}
	rest = Trim(rest.substr(second_space));

	// '<pattern>'
	if (rest.empty() || rest[0] != '\'') {
		return ParserExtensionParseResult("Expected quoted '<pattern>' after method");
	}
	auto pattern_end = rest.find('\'', 1);
	if (pattern_end == string::npos) {
		return ParserExtensionParseResult("Unterminated route pattern");
	}
	auto pattern = rest.substr(1, pattern_end - 1);
	if (pattern.empty() || pattern[0] != '/') {
		return ParserExtensionParseResult("Route pattern must start with '/'");
	}
	rest = Trim(rest.substr(pattern_end + 1));

	// [STATUS <n>]
	int status = 200;
	auto rest_upper = StringUtil::Upper(rest);
	if (StringUtil::StartsWith(rest_upper, "STATUS ")) {
		rest = Trim(rest.substr(7));
		auto space = rest.find(' ');
		if (space == string::npos) {
			return ParserExtensionParseResult("Expected AS <select> after STATUS <n>");
		}
		status = atoi(rest.substr(0, space).c_str());
		if (status < 100 || status > 599) {
			return ParserExtensionParseResult("STATUS must be a valid HTTP status code");
		}
		rest = Trim(rest.substr(space));
		rest_upper = StringUtil::Upper(rest);
	}

	// AS <select>
	if (!StringUtil::StartsWith(rest_upper, "AS ")) {
		return ParserExtensionParseResult("Expected AS <select> in CREATE ROUTE");
	}
	auto handler = Trim(rest.substr(3));
	if (handler.empty()) {
		return ParserExtensionParseResult("Empty handler after AS");
	}

	auto data = make_uniq<RouteDdlParseData>();
	data->action = "CREATE";
	data->or_replace = or_replace;
	data->route.name = name;
	data->route.method = method;
	data->route.pattern = pattern;
	data->route.handler_sql = handler;
	data->route.status = status;
	return ParserExtensionParseResult(std::move(data));
}

//! Execution target for the planned DDL. All side effects happen here, at
//! execution time — plan/bind must not touch the registry (or run SQL: the
//! binder holds the ClientContext lock).
struct ApplyRouteBindData : public TableFunctionData {
	string action;
	bool or_replace = false;
	QuackapiRoute route;
	bool finished = false;
};

unique_ptr<FunctionData> ApplyRouteBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ApplyRouteBindData>();
	bind_data->action = input.inputs[0].GetValue<string>();
	bind_data->or_replace = input.inputs[1].GetValue<bool>();
	bind_data->route.name = input.inputs[2].GetValue<string>();
	bind_data->route.method = input.inputs[3].GetValue<string>();
	bind_data->route.pattern = input.inputs[4].GetValue<string>();
	bind_data->route.handler_sql = input.inputs[5].GetValue<string>();
	bind_data->route.status = input.inputs[6].GetValue<int32_t>();
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("status");
	return std::move(bind_data);
}

void ApplyRouteExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ApplyRouteBindData>();
	if (bind_data.finished) {
		return;
	}
	auto &state = QuackapiState::Get(*context.db);
	string message;
	if (bind_data.action == "CREATE") {
		// Validate the handler SQL now so a broken route fails at CREATE time,
		// not at first request. Do this BEFORE mutating the registry so
		// CREATE OR REPLACE does not leave a half-applied route on failure.
		{
			Connection con(*context.db);
			auto prepared = con.Prepare(bind_data.route.handler_sql);
			if (prepared->HasError()) {
				throw InvalidInputException("Invalid handler SQL for route \"%s\": %s", bind_data.route.name,
				                            prepared->GetError());
			}
		}
		state.AddRoute(bind_data.route, bind_data.or_replace);
		message = StringUtil::Format("Route %s: %s %s", bind_data.route.name, bind_data.route.method,
		                             bind_data.route.pattern);
	} else {
		if (state.DropRoute(bind_data.route.name)) {
			message = StringUtil::Format("Dropped route %s", bind_data.route.name);
		} else {
			throw InvalidInputException("Route \"%s\" does not exist", bind_data.route.name);
		}
	}
	output.SetValue(0, 0, Value(message));
	output.SetCardinality(1);
	bind_data.finished = true;
}

TableFunction MakeApplyRouteFunction() {
	TableFunction function("quackapi_apply_route",
	                       {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                        LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER},
	                       ApplyRouteExec, ApplyRouteBind);
	return function;
}

ParserExtensionPlanResult RouteDdlPlan(ParserExtensionInfo *, ClientContext &,
                                       unique_ptr<ParserExtensionParseData> parse_data) {
	auto &data = static_cast<RouteDdlParseData &>(*parse_data);
	ParserExtensionPlanResult result;
	result.function = MakeApplyRouteFunction();
	result.parameters.push_back(Value(data.action));
	result.parameters.push_back(Value::BOOLEAN(data.or_replace));
	result.parameters.push_back(Value(data.route.name));
	result.parameters.push_back(Value(data.route.method));
	result.parameters.push_back(Value(data.route.pattern));
	result.parameters.push_back(Value(data.route.handler_sql));
	result.parameters.push_back(Value::INTEGER(data.route.status));
	result.requires_valid_transaction = false;
	result.return_type = StatementReturnType::QUERY_RESULT;
	return result;
}

} // namespace

RouteDdlParserExtension::RouteDdlParserExtension() {
	parse_function = RouteDdlParse;
	plan_function = RouteDdlPlan;
}

TableFunction GetApplyRouteFunction() {
	return MakeApplyRouteFunction();
}

} // namespace duckdb
