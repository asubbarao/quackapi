#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parser_extension.hpp"

#include "quackapi_state.hpp"
#include "quackapi_table_api.hpp"

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

//! A quoted '<value>' token starting at rest[0]. Returns the inner value and
//! advances `rest` past the closing quote. Throws a parse-style error via the
//! bool return (false) on a missing/unterminated quote.
bool ParseQuoted(string &rest, string &out) {
	rest = Trim(rest);
	if (rest.empty() || rest[0] != '\'') {
		return false;
	}
	auto close = rest.find('\'', 1);
	if (close == string::npos) {
		return false;
	}
	out = rest.substr(1, close - 1);
	rest = Trim(rest.substr(close + 1));
	return true;
}

//! Parsed CREATE API FOR TABLE, carried from parse to plan.
struct TableApiParseData : public ParserExtensionParseData {
	bool or_replace = false;
	string table;
	string base_path; // empty => default '/<table>'
	string key = "id";

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto copy = make_uniq<TableApiParseData>();
		copy->or_replace = or_replace;
		copy->table = table;
		copy->base_path = base_path;
		copy->key = key;
		return std::move(copy);
	}
	string ToString() const override {
		return "CREATE API FOR TABLE " + table;
	}
};

//! Grammar:
//!   CREATE [OR REPLACE] API FOR TABLE <table> [AT '<base>'] [KEY '<column>']
ParserExtensionParseResult TableApiParse(ParserExtensionInfo *, const string &query) {
	auto q = Trim(query);
	auto upper = StringUtil::Upper(q);

	bool or_replace = false;
	idx_t pos;
	if (StringUtil::StartsWith(upper, "CREATE API FOR TABLE ")) {
		pos = 21;
	} else if (StringUtil::StartsWith(upper, "CREATE OR REPLACE API FOR TABLE ")) {
		pos = 32;
		or_replace = true;
	} else {
		// Not ours — let the core parser (or another extension) handle it.
		return ParserExtensionParseResult();
	}

	auto rest = Trim(q.substr(pos));
	if (rest.empty()) {
		return ParserExtensionParseResult("CREATE API FOR TABLE expects a table name");
	}

	// <table> is the first whitespace-delimited token.
	auto space = rest.find(' ');
	auto data = make_uniq<TableApiParseData>();
	data->or_replace = or_replace;
	if (space == string::npos) {
		data->table = rest;
		rest.clear();
	} else {
		data->table = rest.substr(0, space);
		rest = Trim(rest.substr(space));
	}
	if (data->table.empty() || data->table.find('\'') != string::npos) {
		return ParserExtensionParseResult("CREATE API FOR TABLE: invalid table name");
	}

	// Optional clauses in any order: AT '<base>', KEY '<column>'
	while (!rest.empty()) {
		auto rest_upper = StringUtil::Upper(rest);
		if (StringUtil::StartsWith(rest_upper, "AT ")) {
			rest = rest.substr(3);
			if (!ParseQuoted(rest, data->base_path) || data->base_path.empty() || data->base_path[0] != '/') {
				return ParserExtensionParseResult("CREATE API FOR TABLE: AT expects a quoted '/path'");
			}
		} else if (StringUtil::StartsWith(rest_upper, "KEY ")) {
			rest = rest.substr(4);
			if (!ParseQuoted(rest, data->key) || data->key.empty()) {
				return ParserExtensionParseResult("CREATE API FOR TABLE: KEY expects a quoted 'column'");
			}
		} else {
			return ParserExtensionParseResult("CREATE API FOR TABLE: unexpected clause \"" + rest + "\"");
		}
	}

	return ParserExtensionParseResult(std::move(data));
}

struct ApplyApiBindData : public TableFunctionData {
	bool or_replace = false;
	string table;
	string base_path;
	string key;
	bool finished = false;
};

unique_ptr<FunctionData> ApplyApiBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                      vector<string> &names) {
	auto bind_data = make_uniq<ApplyApiBindData>();
	bind_data->or_replace = input.inputs[0].GetValue<bool>();
	bind_data->table = input.inputs[1].GetValue<string>();
	bind_data->base_path = input.inputs[2].GetValue<string>();
	bind_data->key = input.inputs[3].GetValue<string>();
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("status");
	return std::move(bind_data);
}

//! Backtick-safe identifier quoting for embedding a table/column into generated
//! handler SQL (double-quote, doubling any embedded quote).
string QuoteIdent(const string &ident) {
	string out = "\"";
	for (char c : ident) {
		if (c == '"') {
			out += "\"\"";
		} else {
			out += c;
		}
	}
	out += "\"";
	return out;
}

void ApplyApiExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ApplyApiBindData>();
	if (bind_data.finished) {
		return;
	}
	auto &state = QuackapiState::Get(*context.db);

	auto base = bind_data.base_path.empty() ? ("/" + bind_data.table) : bind_data.base_path;
	// Normalize: no trailing slash (except the root itself).
	while (base.size() > 1 && base.back() == '/') {
		base.pop_back();
	}
	auto quoted_table = QuoteIdent(bind_data.table);
	auto quoted_key = QuoteIdent(bind_data.key);

	// GET <base>  -> list
	QuackapiRoute list_route;
	list_route.name = bind_data.table + "_list";
	list_route.method = "GET";
	list_route.pattern = base;
	list_route.handler_sql = "SELECT * FROM " + quoted_table;
	list_route.status = 200;
	state.AddRoute(list_route, bind_data.or_replace);

	// GET <base>/:<key>  -> by key
	QuackapiRoute get_route;
	get_route.name = bind_data.table + "_get";
	get_route.method = "GET";
	get_route.pattern = base + "/:" + bind_data.key;
	get_route.handler_sql =
	    "SELECT * FROM " + quoted_table + " WHERE " + quoted_key + " = $" + bind_data.key;
	get_route.status = 200;
	state.AddRoute(get_route, bind_data.or_replace);

	output.SetValue(0, 0,
	                Value(StringUtil::Format("API for %s: GET %s, GET %s/:%s", bind_data.table, base, base,
	                                         bind_data.key)));
	output.SetCardinality(1);
	bind_data.finished = true;
}

TableFunction MakeApplyApiFunction() {
	return TableFunction("quackapi_apply_table_api",
	                     {LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                     ApplyApiExec, ApplyApiBind);
}

ParserExtensionPlanResult TableApiPlan(ParserExtensionInfo *, ClientContext &,
                                       unique_ptr<ParserExtensionParseData> parse_data) {
	auto &data = static_cast<TableApiParseData &>(*parse_data);
	ParserExtensionPlanResult result;
	result.function = MakeApplyApiFunction();
	result.parameters.push_back(Value::BOOLEAN(data.or_replace));
	result.parameters.push_back(Value(data.table));
	result.parameters.push_back(Value(data.base_path));
	result.parameters.push_back(Value(data.key));
	result.requires_valid_transaction = false;
	result.return_type = StatementReturnType::QUERY_RESULT;
	return result;
}

} // namespace

TableApiDdlParserExtension::TableApiDdlParserExtension() {
	parse_function = TableApiParse;
	plan_function = TableApiPlan;
}

} // namespace duckdb
