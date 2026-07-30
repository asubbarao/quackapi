#include "quackapi_graphql.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/parser/parser_extension.hpp"

#include "quackapi_state.hpp"
#include "quackapi_util.hpp"

namespace duckdb {

namespace {

string GraphqlError(const string &message) {
	return "{\"errors\":[{\"message\":\"" + QuackapiJsonEscape(message) + "\"}]}";
}

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

//! GraphQL Name: [_A-Za-z][_0-9A-Za-z]* — character scan, no regex.
bool IsGraphqlNameStart(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
bool IsGraphqlNameCont(char c) {
	return IsGraphqlNameStart(c) || (c >= '0' && c <= '9');
}

void SkipWs(const string &s, idx_t &i) {
	while (i < s.size()) {
		char c = s[i];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',') {
			// GraphQL allows commas as insignificant separators.
			i++;
			continue;
		}
		// Line comments: # … EOL
		if (c == '#') {
			while (i < s.size() && s[i] != '\n') {
				i++;
			}
			continue;
		}
		break;
	}
}

bool MatchChar(const string &s, idx_t &i, char expected) {
	SkipWs(s, i);
	if (i < s.size() && s[i] == expected) {
		i++;
		return true;
	}
	return false;
}

bool ReadName(const string &s, idx_t &i, string &out) {
	SkipWs(s, i);
	if (i >= s.size() || !IsGraphqlNameStart(s[i])) {
		return false;
	}
	idx_t start = i;
	i++;
	while (i < s.size() && IsGraphqlNameCont(s[i])) {
		i++;
	}
	out = s.substr(start, i - start);
	return true;
}

struct GraphqlField {
	string name;
	vector<string> columns; // empty = invalid for v0 (need selection set)
};

struct GraphqlDocument {
	vector<GraphqlField> roots;
	string error;
};

//! Parse only: [query [Name]] { Field { Name+ }+ }
//! Field must have a nested selection set of leaf column names (no deeper nest).
GraphqlDocument ParseThinGraphql(const string &doc) {
	GraphqlDocument out;
	idx_t i = 0;
	SkipWs(doc, i);
	if (i >= doc.size()) {
		out.error = "empty GraphQL document";
		return out;
	}

	// Optional operation: query [Name]
	string first;
	idx_t save = i;
	if (ReadName(doc, i, first)) {
		if (first == "query") {
			// Optional operation name — skip one Name if present.
			string op_name;
			idx_t before = i;
			if (ReadName(doc, i, op_name)) {
				// If next significant char is '{', op_name was the operation name.
				// If we mis-consumed a field name (no op name, bare `{ field {…} }`
				// after a mistaken "query" keyword path), we only enter this branch
				// when the keyword was literally "query".
				SkipWs(doc, i);
				if (i < doc.size() && doc[i] == '{') {
					// op_name was optional name — fine, leave i at '{'
				} else {
					// Not a selection set after the name — treat as error.
					out.error = "expected selection set '{' after query";
					return out;
				}
			} else {
				i = before;
			}
		} else if (first == "mutation" || first == "subscription") {
			out.error = "mutations and subscriptions are not supported in GraphQL v0";
			return out;
		} else {
			// Leading name without 'query' keyword — not valid unless we rewind
			// and expect '{'. Rewind: document must start with '{'.
			i = save;
		}
	}

	if (!MatchChar(doc, i, '{')) {
		out.error = "expected selection set '{'";
		return out;
	}

	while (true) {
		SkipWs(doc, i);
		if (i < doc.size() && doc[i] == '}') {
			i++;
			break;
		}
		string field_name;
		if (!ReadName(doc, i, field_name)) {
			out.error = "expected field name in root selection set";
			return out;
		}
		if (field_name == "__schema" || field_name == "__type") {
			out.error = "introspection fields are not supported in v0; use GET /graphql/schema";
			return out;
		}
		if (!MatchChar(doc, i, '{')) {
			out.error = "v0 requires a column selection set: { " + field_name + " { col1 col2 } }";
			return out;
		}
		GraphqlField field;
		field.name = field_name;
		while (true) {
			SkipWs(doc, i);
			if (i < doc.size() && doc[i] == '}') {
				i++;
				break;
			}
			string col;
			if (!ReadName(doc, i, col)) {
				out.error = "expected column name inside " + field_name;
				return out;
			}
			// No nested selection under columns in v0.
			SkipWs(doc, i);
			if (i < doc.size() && doc[i] == '{') {
				out.error = "nested selections are not supported in GraphQL v0";
				return out;
			}
			if (i < doc.size() && doc[i] == '(') {
				out.error = "field arguments are not supported in GraphQL v0";
				return out;
			}
			field.columns.push_back(col);
		}
		if (field.columns.empty()) {
			out.error = "selection set for " + field_name + " must list at least one column";
			return out;
		}
		out.roots.push_back(std::move(field));
	}

	SkipWs(doc, i);
	if (i < doc.size()) {
		out.error = "unexpected trailing content after selection set";
		return out;
	}
	if (out.roots.empty()) {
		out.error = "selection set is empty";
		return out;
	}
	return out;
}

bool TableExists(Connection &con, const string &table) {
	// Catalog truth — schema main, non-internal. Case-sensitive match on stored name.
	auto res = con.Query("SELECT 1 FROM duckdb_tables() WHERE schema_name = 'main' AND NOT internal "
	                     "AND table_name = ? LIMIT 1",
	                     Value(table));
	if (res->HasError()) {
		return false;
	}
	auto chunk = res->Fetch();
	return chunk && chunk->size() > 0;
}

bool ColumnExists(Connection &con, const string &table, const string &column) {
	auto res = con.Query("SELECT 1 FROM duckdb_columns() WHERE schema_name = 'main' AND NOT internal "
	                     "AND table_name = ? AND column_name = ? LIMIT 1",
	                     Value(table), Value(column));
	if (res->HasError()) {
		return false;
	}
	auto chunk = res->Fetch();
	return chunk && chunk->size() > 0;
}

//! SELECT cols… FROM table LIMIT n → JSON array of row objects via DuckDB JSON.
string SelectTableJson(Connection &con, const string &table, const vector<string> &columns, idx_t limit, string &err) {
	string select_list;
	for (idx_t c = 0; c < columns.size(); c++) {
		if (c > 0) {
			select_list += ", ";
		}
		select_list += QuoteIdent(columns[c]);
	}
	// Build json_object('col', "col", …) so types follow DuckDB → JSON.
	string json_obj = "json_object(";
	for (idx_t c = 0; c < columns.size(); c++) {
		if (c > 0) {
			json_obj += ", ";
		}
		json_obj += "'" + StringUtil::Replace(columns[c], "'", "''") + "', " + QuoteIdent(columns[c]);
	}
	json_obj += ")";

	string sql = "SELECT coalesce(json_group_array(" + json_obj +
	             "), '[]'::JSON)::VARCHAR FROM ("
	             "SELECT " +
	             select_list + " FROM " + QuoteIdent(table) + " LIMIT " + std::to_string(limit) + ") _gql";

	auto res = con.Query(sql);
	if (res->HasError()) {
		err = res->GetError();
		return {};
	}
	auto chunk = res->Fetch();
	if (!chunk || chunk->size() == 0 || chunk->GetValue(0, 0).IsNull()) {
		return "[]";
	}
	return chunk->GetValue(0, 0).ToString();
}

//===--------------------------------------------------------------------===//
// CREATE / DROP GRAPHQL FOR TABLE (allowlist for built-in /graphql)
//===--------------------------------------------------------------------===//

//! Parse a SQL identifier: bare token OR double-quoted with "" escapes.
bool ParseIdent(string &rest, string &out) {
	rest = QuackapiTrim(rest);
	if (rest.empty()) {
		return false;
	}
	if (rest[0] == '"') {
		string result;
		idx_t i = 1;
		while (i < rest.size()) {
			if (rest[i] == '"') {
				if (i + 1 < rest.size() && rest[i + 1] == '"') {
					result += '"';
					i += 2;
					continue;
				}
				out = result;
				rest = QuackapiTrim(rest.substr(i + 1));
				return !out.empty();
			}
			result += rest[i];
			i++;
		}
		return false; // unterminated
	}
	// Bare identifier: up to whitespace or comma.
	idx_t i = 0;
	while (i < rest.size() && !StringUtil::CharacterIsSpace(rest[i]) && rest[i] != ',') {
		if (rest[i] == '"') {
			return false;
		}
		i++;
	}
	if (i == 0) {
		return false;
	}
	out = rest.substr(0, i);
	rest = QuackapiTrim(rest.substr(i));
	return true;
}

bool ParseTableList(string &rest, vector<string> &tables, string &err) {
	tables.clear();
	while (true) {
		string name;
		if (!ParseIdent(rest, name)) {
			err = "invalid table name — use a bare identifier or \"quoted\"\"ident\"";
			return false;
		}
		if (name.find('\'') != string::npos) {
			err = "invalid table name";
			return false;
		}
		tables.push_back(name);
		rest = QuackapiTrim(rest);
		if (rest.empty()) {
			return true;
		}
		if (rest[0] == ',') {
			rest = QuackapiTrim(rest.substr(1));
			if (rest.empty()) {
				err = "trailing comma in table list";
				return false;
			}
			continue;
		}
		err = "unexpected trailing content after table list";
		return false;
	}
}

struct GraphqlDdlParseData : public ParserExtensionParseData {
	//! "CREATE" | "DROP" | "CLEAR" | "CREATE_ROUTE" | "DROP_ROUTE"
	string action;
	bool or_replace = false;
	vector<string> tables;
	//! CREATE/DROP GRAPHQL ROUTE fields
	string route_name;
	string method = "POST";
	string path;
	string require_auth;
	idx_t limit = QUACKAPI_GRAPHQL_DEFAULT_LIMIT;
	bool has_limit = false;

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto copy = make_uniq<GraphqlDdlParseData>();
		copy->action = action;
		copy->or_replace = or_replace;
		copy->tables = tables;
		copy->route_name = route_name;
		copy->method = method;
		copy->path = path;
		copy->require_auth = require_auth;
		copy->limit = limit;
		copy->has_limit = has_limit;
		return std::move(copy);
	}
	string ToString() const override {
		if (action == "CLEAR") {
			return "DROP GRAPHQL ALL";
		}
		if (action == "DROP_ROUTE") {
			return "DROP GRAPHQL ROUTE " + route_name;
		}
		if (action == "CREATE_ROUTE") {
			string s = string(or_replace ? "CREATE OR REPLACE" : "CREATE") + " GRAPHQL ROUTE " + route_name + " " +
			           method + " '" + path + "' FROM";
			for (idx_t i = 0; i < tables.size(); i++) {
				s += (i == 0 ? " " : ", ") + tables[i];
			}
			if (!require_auth.empty()) {
				s += " REQUIRE " + require_auth;
			}
			if (has_limit) {
				s += " LIMIT " + std::to_string(limit);
			}
			return s;
		}
		string s = action + " GRAPHQL FOR TABLE";
		for (idx_t i = 0; i < tables.size(); i++) {
			s += (i == 0 ? " " : ", ") + tables[i];
		}
		return s;
	}
};

//! Parse table list that may be followed by REQUIRE / LIMIT keywords.
bool ParseTableListUntilKeywords(string &rest, vector<string> &tables, string &err) {
	tables.clear();
	while (true) {
		rest = QuackapiTrim(rest);
		if (rest.empty()) {
			if (tables.empty()) {
				err = "expected at least one table name";
				return false;
			}
			return true;
		}
		auto upper = StringUtil::Upper(rest);
		// Stop before optional clauses.
		if ((StringUtil::StartsWith(upper, "REQUIRE") &&
		     (rest.size() == 7 || StringUtil::CharacterIsSpace(rest[7]))) ||
		    (StringUtil::StartsWith(upper, "LIMIT") &&
		     (rest.size() == 5 || StringUtil::CharacterIsSpace(rest[5])))) {
			if (tables.empty()) {
				err = "expected at least one table name before " + rest.substr(0, 6);
				return false;
			}
			return true;
		}
		string name;
		if (!ParseIdent(rest, name)) {
			err = "invalid table name — use a bare identifier or \"quoted\"\"ident\"";
			return false;
		}
		if (name.find('\'') != string::npos) {
			err = "invalid table name";
			return false;
		}
		tables.push_back(name);
		rest = QuackapiTrim(rest);
		if (rest.empty()) {
			return true;
		}
		if (rest[0] == ',') {
			rest = QuackapiTrim(rest.substr(1));
			if (rest.empty()) {
				err = "trailing comma in table list";
				return false;
			}
			continue;
		}
		// Next token may be REQUIRE/LIMIT — loop will stop.
		upper = StringUtil::Upper(rest);
		if ((StringUtil::StartsWith(upper, "REQUIRE") &&
		     (rest.size() == 7 || StringUtil::CharacterIsSpace(rest[7]))) ||
		    (StringUtil::StartsWith(upper, "LIMIT") &&
		     (rest.size() == 5 || StringUtil::CharacterIsSpace(rest[5])))) {
			return true;
		}
		err = "unexpected trailing content after table list";
		return false;
	}
}

//! Grammar:
//!   CREATE [OR REPLACE] GRAPHQL FOR TABLE <table> [, <table> ...]
//!   DROP GRAPHQL FOR TABLE <table> [, <table> ...]
//!   DROP GRAPHQL ALL
//!   CREATE [OR REPLACE] GRAPHQL ROUTE <name> POST '<path>' FROM <tables>
//!        [REQUIRE <auth>] [LIMIT <n>]
//!   DROP GRAPHQL ROUTE <name>
ParserExtensionParseResult GraphqlDdlParse(ParserExtensionInfo *, const string &query) {
	auto q = QuackapiTrim(query);
	auto upper = StringUtil::Upper(q);

	// DROP GRAPHQL ALL — token boundary so "DROP GRAPHQL ALLOWED" is not ours.
	if (StringUtil::StartsWith(upper, "DROP GRAPHQL ALL") &&
	    (upper.size() == 16 || (!StringUtil::CharacterIsAlphaNumeric(upper[16]) && upper[16] != '_'))) {
		auto tail = QuackapiTrim(q.substr(16));
		if (!tail.empty()) {
			return ParserExtensionParseResult("DROP GRAPHQL ALL takes no arguments");
		}
		auto data = make_uniq<GraphqlDdlParseData>();
		data->action = "CLEAR";
		return ParserExtensionParseResult(std::move(data));
	}

	// DROP GRAPHQL ROUTE <name>
	if (StringUtil::StartsWith(upper, "DROP GRAPHQL ROUTE ")) {
		auto name = QuackapiTrim(q.substr(19));
		if (name.empty() || name.find(' ') != string::npos) {
			return ParserExtensionParseResult("DROP GRAPHQL ROUTE expects a single route name");
		}
		auto data = make_uniq<GraphqlDdlParseData>();
		data->action = "DROP_ROUTE";
		data->route_name = name;
		return ParserExtensionParseResult(std::move(data));
	}

	// CREATE [OR REPLACE] GRAPHQL ROUTE …
	bool route_or_replace = false;
	idx_t route_pos = 0;
	if (StringUtil::StartsWith(upper, "CREATE GRAPHQL ROUTE ")) {
		route_pos = 21;
	} else if (StringUtil::StartsWith(upper, "CREATE OR REPLACE GRAPHQL ROUTE ")) {
		route_pos = 32;
		route_or_replace = true;
	}
	if (route_pos > 0) {
		auto rest = QuackapiTrim(q.substr(route_pos));
		// <name>
		auto sp = rest.find(' ');
		if (sp == string::npos) {
			return ParserExtensionParseResult(
			    "CREATE GRAPHQL ROUTE <name> POST '<path>' FROM <table> [, …]");
		}
		auto name = rest.substr(0, sp);
		rest = QuackapiTrim(rest.substr(sp));
		// POST
		sp = rest.find(' ');
		if (sp == string::npos) {
			return ParserExtensionParseResult("Expected POST '<path>' after GRAPHQL ROUTE name");
		}
		auto method = StringUtil::Upper(rest.substr(0, sp));
		if (method != "POST") {
			return ParserExtensionParseResult(
			    "CREATE GRAPHQL ROUTE only supports POST in v0 (got " + method + ")");
		}
		rest = QuackapiTrim(rest.substr(sp));
		// '<path>'
		if (rest.empty() || rest[0] != '\'') {
			return ParserExtensionParseResult("Expected quoted '<path>' after POST");
		}
		auto path_end = rest.find('\'', 1);
		if (path_end == string::npos) {
			return ParserExtensionParseResult("Unterminated GraphQL route path");
		}
		auto path = rest.substr(1, path_end - 1);
		if (path.empty() || path[0] != '/') {
			return ParserExtensionParseResult("GraphQL route path must be absolute (start with '/')");
		}
		rest = QuackapiTrim(rest.substr(path_end + 1));
		// FROM
		auto rest_u = StringUtil::Upper(rest);
		if (!StringUtil::StartsWith(rest_u, "FROM") ||
		    !(rest.size() == 4 || StringUtil::CharacterIsSpace(rest[4]))) {
			return ParserExtensionParseResult("Expected FROM <table> [, …] after path");
		}
		rest = QuackapiTrim(rest.substr(4));
		vector<string> tables;
		string err;
		if (!ParseTableListUntilKeywords(rest, tables, err)) {
			return ParserExtensionParseResult("CREATE GRAPHQL ROUTE: " + err);
		}
		string require_auth;
		idx_t limit = QUACKAPI_GRAPHQL_DEFAULT_LIMIT;
		bool has_limit = false;
		for (int round = 0; round < 4; round++) {
			rest = QuackapiTrim(rest);
			if (rest.empty()) {
				break;
			}
			rest_u = StringUtil::Upper(rest);
			if (StringUtil::StartsWith(rest_u, "REQUIRE") &&
			    (rest.size() == 7 || StringUtil::CharacterIsSpace(rest[7]))) {
				rest = QuackapiTrim(rest.substr(7));
				idx_t te = 0;
				while (te < rest.size() && !StringUtil::CharacterIsSpace(rest[te])) {
					te++;
				}
				if (te == 0) {
					return ParserExtensionParseResult("REQUIRE expects an auth name");
				}
				require_auth = rest.substr(0, te);
				rest = QuackapiTrim(rest.substr(te));
				continue;
			}
			if (StringUtil::StartsWith(rest_u, "LIMIT") &&
			    (rest.size() == 5 || StringUtil::CharacterIsSpace(rest[5]))) {
				rest = QuackapiTrim(rest.substr(5));
				idx_t te = 0;
				while (te < rest.size() && !StringUtil::CharacterIsSpace(rest[te])) {
					te++;
				}
				if (te == 0) {
					return ParserExtensionParseResult("LIMIT expects a positive integer");
				}
				auto lim_s = rest.substr(0, te);
				for (char c : lim_s) {
					if (c < '0' || c > '9') {
						return ParserExtensionParseResult("LIMIT expects a positive integer");
					}
				}
				limit = (idx_t)std::stoull(lim_s);
				if (limit == 0 || limit > 100000) {
					return ParserExtensionParseResult("LIMIT must be between 1 and 100000");
				}
				has_limit = true;
				rest = QuackapiTrim(rest.substr(te));
				continue;
			}
			return ParserExtensionParseResult("Unexpected trailing content after GRAPHQL ROUTE: " + rest);
		}
		auto data = make_uniq<GraphqlDdlParseData>();
		data->action = "CREATE_ROUTE";
		data->or_replace = route_or_replace;
		data->route_name = name;
		data->method = method;
		data->path = path;
		data->tables = std::move(tables);
		data->require_auth = require_auth;
		data->limit = limit;
		data->has_limit = has_limit;
		return ParserExtensionParseResult(std::move(data));
	}

	bool or_replace = false;
	bool is_drop = false;
	idx_t pos = 0;
	if (StringUtil::StartsWith(upper, "CREATE GRAPHQL FOR TABLE ")) {
		pos = 25;
	} else if (StringUtil::StartsWith(upper, "CREATE OR REPLACE GRAPHQL FOR TABLE ")) {
		pos = 36;
		or_replace = true;
	} else if (StringUtil::StartsWith(upper, "DROP GRAPHQL FOR TABLE ")) {
		pos = 23;
		is_drop = true;
	} else {
		// Not ours — let the core parser (or another extension) handle it.
		return ParserExtensionParseResult();
	}

	auto rest = QuackapiTrim(q.substr(pos));
	if (rest.empty()) {
		return ParserExtensionParseResult(
		    is_drop ? "DROP GRAPHQL FOR TABLE expects at least one table name"
		            : "CREATE GRAPHQL FOR TABLE expects at least one table name");
	}

	vector<string> tables;
	string err;
	if (!ParseTableList(rest, tables, err)) {
		return ParserExtensionParseResult(string(is_drop ? "DROP" : "CREATE") + " GRAPHQL FOR TABLE: " + err);
	}

	auto data = make_uniq<GraphqlDdlParseData>();
	data->action = is_drop ? "DROP" : "CREATE";
	data->or_replace = or_replace;
	data->tables = std::move(tables);
	return ParserExtensionParseResult(std::move(data));
}

struct ApplyGraphqlBindData : public TableFunctionData {
	string action;
	bool or_replace = false;
	vector<string> tables;
	string route_name;
	string method = "POST";
	string path;
	string require_auth;
	idx_t limit = QUACKAPI_GRAPHQL_DEFAULT_LIMIT;
	bool finished = false;
};

unique_ptr<FunctionData> ApplyGraphqlBind(ClientContext &, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ApplyGraphqlBindData>();
	bind_data->action = input.inputs[0].GetValue<string>();
	bind_data->or_replace = input.inputs[1].GetValue<bool>();
	// tables as VARCHAR[] (LIST)
	if (!input.inputs[2].IsNull() && input.inputs[2].type().id() == LogicalTypeId::LIST) {
		for (auto &child : ListValue::GetChildren(input.inputs[2])) {
			if (!child.IsNull()) {
				bind_data->tables.push_back(child.ToString());
			}
		}
	}
	if (input.inputs.size() > 3 && !input.inputs[3].IsNull()) {
		bind_data->route_name = input.inputs[3].GetValue<string>();
	}
	if (input.inputs.size() > 4 && !input.inputs[4].IsNull()) {
		bind_data->method = input.inputs[4].GetValue<string>();
	}
	if (input.inputs.size() > 5 && !input.inputs[5].IsNull()) {
		bind_data->path = input.inputs[5].GetValue<string>();
	}
	if (input.inputs.size() > 6 && !input.inputs[6].IsNull()) {
		bind_data->require_auth = input.inputs[6].GetValue<string>();
	}
	if (input.inputs.size() > 7 && !input.inputs[7].IsNull()) {
		bind_data->limit = (idx_t)input.inputs[7].GetValue<int64_t>();
	}
	BindStatusColumn(return_types, names);
	return std::move(bind_data);
}

void ApplyGraphqlExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ApplyGraphqlBindData>();
	if (bind_data.finished) {
		return;
	}
	auto &state = QuackapiState::Get(*context.db);
	Connection con(*context.db);

	if (bind_data.action == "CLEAR") {
		state.ClearGraphqlTables();
		EmitOneShotStatus(output, bind_data.finished, "GraphQL allowlist cleared (open catalog mode)");
		return;
	}

	if (bind_data.action == "CREATE_ROUTE") {
		for (auto &table : bind_data.tables) {
			if (!TableExists(con, table)) {
				throw InvalidInputException(
				    "CREATE GRAPHQL ROUTE: table or view \"%s\" not found in schema main", table);
			}
		}
		if (!bind_data.require_auth.empty()) {
			QuackapiAuth auth;
			if (!state.GetAuth(bind_data.require_auth, auth)) {
				throw InvalidInputException("CREATE GRAPHQL ROUTE: auth scheme \"%s\" does not exist",
				                            bind_data.require_auth);
			}
		}
		QuackapiGraphqlRoute route;
		route.name = bind_data.route_name;
		route.method = bind_data.method;
		route.path = bind_data.path;
		route.tables = bind_data.tables;
		route.require_auth = bind_data.require_auth;
		route.limit = bind_data.limit == 0 ? QUACKAPI_GRAPHQL_DEFAULT_LIMIT : bind_data.limit;
		state.AddGraphqlRoute(route, bind_data.or_replace);
		string msg = "GraphQL route " + route.name + ": " + route.method + " " + route.path + " FROM ";
		for (idx_t i = 0; i < route.tables.size(); i++) {
			if (i > 0) {
				msg += ", ";
			}
			msg += route.tables[i];
		}
		if (!route.require_auth.empty()) {
			msg += " REQUIRE " + route.require_auth;
		}
		msg += " LIMIT " + std::to_string(route.limit);
		EmitOneShotStatus(output, bind_data.finished, msg);
		return;
	}

	if (bind_data.action == "DROP_ROUTE") {
		if (state.DropGraphqlRoute(bind_data.route_name)) {
			EmitOneShotStatus(output, bind_data.finished, "Dropped GraphQL route " + bind_data.route_name);
		} else {
			throw InvalidInputException("GraphQL route \"%s\" does not exist", bind_data.route_name);
		}
		return;
	}

	if (bind_data.action == "CREATE") {
		for (auto &table : bind_data.tables) {
			if (!TableExists(con, table)) {
				throw InvalidInputException(
				    "CREATE GRAPHQL FOR TABLE: table or view \"%s\" not found in schema main", table);
			}
			state.AddGraphqlTable(table, bind_data.or_replace);
		}
		string msg = "GraphQL registered: ";
		for (idx_t i = 0; i < bind_data.tables.size(); i++) {
			if (i > 0) {
				msg += ", ";
			}
			msg += bind_data.tables[i];
		}
		msg += " (allowlist mode on POST /graphql)";
		EmitOneShotStatus(output, bind_data.finished, msg);
		return;
	}

	if (bind_data.action == "DROP") {
		vector<string> dropped;
		vector<string> missing;
		for (auto &table : bind_data.tables) {
			if (state.DropGraphqlTable(table)) {
				dropped.push_back(table);
			} else {
				missing.push_back(table);
			}
		}
		if (dropped.empty()) {
			throw InvalidInputException("DROP GRAPHQL FOR TABLE: no matching registered tables");
		}
		string msg = "GraphQL unregistered: ";
		for (idx_t i = 0; i < dropped.size(); i++) {
			if (i > 0) {
				msg += ", ";
			}
			msg += dropped[i];
		}
		if (!state.GraphqlAllowlistActive()) {
			msg += " (allowlist empty → open catalog mode)";
		}
		if (!missing.empty()) {
			msg += "; not registered: ";
			for (idx_t i = 0; i < missing.size(); i++) {
				if (i > 0) {
					msg += ", ";
				}
				msg += missing[i];
			}
		}
		EmitOneShotStatus(output, bind_data.finished, msg);
		return;
	}

	throw InvalidInputException("internal: unknown GraphQL DDL action \"%s\"", bind_data.action);
}

TableFunction MakeApplyGraphqlFunction() {
	return MakeApplyDdlFunction(
	    "quackapi_apply_graphql",
	    {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::LIST(LogicalType::VARCHAR), LogicalType::VARCHAR,
	     LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT},
	    ApplyGraphqlExec, ApplyGraphqlBind);
}

ParserExtensionPlanResult GraphqlDdlPlan(ParserExtensionInfo *, ClientContext &,
                                         unique_ptr<ParserExtensionParseData> parse_data) {
	auto &data = static_cast<GraphqlDdlParseData &>(*parse_data);
	ParserExtensionPlanResult result;
	result.function = MakeApplyGraphqlFunction();
	result.parameters.push_back(Value(data.action));
	result.parameters.push_back(Value::BOOLEAN(data.or_replace));
	vector<Value> table_vals;
	for (auto &t : data.tables) {
		table_vals.emplace_back(t);
	}
	result.parameters.push_back(Value::LIST(LogicalType::VARCHAR, table_vals));
	result.parameters.push_back(Value(data.route_name));
	result.parameters.push_back(Value(data.method));
	result.parameters.push_back(Value(data.path));
	result.parameters.push_back(Value(data.require_auth));
	result.parameters.push_back(Value::BIGINT((int64_t)data.limit));
	FinishDdlPlan(result);
	return result;
}

//===--------------------------------------------------------------------===//
// quackapi_graphql_tables() — inspect allowlist
//===--------------------------------------------------------------------===//

struct GraphqlTablesBindData : public TableFunctionData {};

struct GraphqlTablesGlobalState : public GlobalTableFunctionState {
	vector<string> tables;
	bool allowlist_active = false;
	idx_t offset = 0;
	bool emitted_open = false;
};

unique_ptr<FunctionData> GraphqlTablesBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                           vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("mode");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("table_name");
	return make_uniq<GraphqlTablesBindData>();
}

unique_ptr<GlobalTableFunctionState> GraphqlTablesInit(ClientContext &context, TableFunctionInitInput &) {
	auto state = make_uniq<GraphqlTablesGlobalState>();
	auto &api = QuackapiState::Get(*context.db);
	state->tables = api.SnapshotGraphqlTables();
	state->allowlist_active = !state->tables.empty();
	return std::move(state);
}

void GraphqlTablesExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<GraphqlTablesGlobalState>();
	if (!state.allowlist_active) {
		// One informational row: open mode, no table_name.
		if (state.emitted_open) {
			return;
		}
		output.SetValue(0, 0, Value("open"));
		output.SetValue(1, 0, Value());
		output.SetCardinality(1);
		state.emitted_open = true;
		return;
	}
	idx_t row = 0;
	while (state.offset < state.tables.size() && row < STANDARD_VECTOR_SIZE) {
		output.SetValue(0, row, Value("allowlist"));
		output.SetValue(1, row, Value(state.tables[state.offset]));
		row++;
		state.offset++;
	}
	output.SetCardinality(row);
}

} // namespace

bool GraphqlExtractQuery(DatabaseInstance &db, const string &raw_body, string &query_out, string &error_json) {
	if (raw_body.empty()) {
		error_json = GraphqlError("request body is empty; expected JSON {\"query\":\"...\"}");
		return false;
	}
	Connection con(db);
	auto res = con.Query("SELECT CASE"
	                     "  WHEN TRY_CAST(? AS JSON) IS NULL THEN NULL"
	                     "  WHEN json_type(?::JSON) != 'OBJECT' THEN NULL"
	                     "  ELSE json_extract_string(?::JSON, '$.query')"
	                     " END",
	                     Value(raw_body), Value(raw_body), Value(raw_body));
	if (res->HasError()) {
		error_json = GraphqlError("invalid JSON body");
		return false;
	}
	auto chunk = res->Fetch();
	if (!chunk || chunk->size() == 0 || chunk->GetValue(0, 0).IsNull()) {
		error_json = GraphqlError("JSON body must be an object with a non-null \"query\" string");
		return false;
	}
	query_out = chunk->GetValue(0, 0).ToString();
	if (query_out.empty()) {
		error_json = GraphqlError("\"query\" must be a non-empty string");
		return false;
	}
	return true;
}

bool TableInList(const vector<string> &tables, const string &name) {
	for (auto &t : tables) {
		if (t == name) {
			return true;
		}
	}
	return false;
}

string ExecuteGraphqlQuery(DatabaseInstance &db, const string &query, const GraphqlExecOptions &options) {
	auto doc = ParseThinGraphql(query);
	if (!doc.error.empty()) {
		return GraphqlError(doc.error);
	}
	idx_t limit = options.limit;
	if (limit == 0) {
		limit = QUACKAPI_GRAPHQL_DEFAULT_LIMIT;
	}

	auto &state = QuackapiState::Get(db);
	const bool route_mode = options.allowed_tables != nullptr;
	const bool global_allowlist = !route_mode && state.GraphqlAllowlistActive();

	Connection con(db);
	string data = "{";
	bool first = true;
	for (auto &field : doc.roots) {
		if (route_mode) {
			if (!TableInList(*options.allowed_tables, field.name)) {
				return GraphqlError("table '" + field.name +
				                    "' is not on this GraphQL route — CREATE GRAPHQL ROUTE … FROM " + field.name);
			}
		} else if (global_allowlist && !state.IsGraphqlTableAllowed(field.name)) {
			return GraphqlError("table '" + field.name +
			                    "' is not registered for GraphQL — CREATE GRAPHQL FOR TABLE " + field.name);
		}
		if (!TableExists(con, field.name)) {
			return GraphqlError("unknown table '" + field.name + "' (main schema catalog only)");
		}
		for (auto &col : field.columns) {
			if (!ColumnExists(con, field.name, col)) {
				return GraphqlError("unknown column '" + col + "' on table '" + field.name + "'");
			}
		}
		string err;
		string rows_json = SelectTableJson(con, field.name, field.columns, limit, err);
		if (!err.empty()) {
			return GraphqlError("query failed for '" + field.name + "': " + err);
		}
		if (!first) {
			data += ",";
		}
		first = false;
		data += "\"" + QuackapiJsonEscape(field.name) + "\":" + rows_json;
	}
	data += "}";
	return "{\"data\":" + data + "}";
}

string GraphqlSchemaNote() {
	return "GraphQL v0 catalog-only schema. Full __schema introspection not implemented. "
	       "CREATE GRAPHQL FOR TABLE registers an allowlist for POST /graphql; empty = open. "
	       "CREATE GRAPHQL ROUTE mounts named paths with per-route tables.";
}

string BuildGraphqlSchema(DatabaseInstance &db, const GraphqlExecOptions &options) {
	Connection con(db);
	auto &state = QuackapiState::Get(db);
	const bool route_mode = options.allowed_tables != nullptr;
	const bool allowlist = !route_mode && state.GraphqlAllowlistActive();
	vector<string> allowed;
	if (route_mode) {
		allowed = *options.allowed_tables;
	} else if (allowlist) {
		allowed = state.SnapshotGraphqlTables();
	}

	// Group columns per table in main schema. Views included (duckdb_tables covers both).
	auto res = con.Query("SELECT t.table_name, "
	                     "coalesce(list(c.column_name ORDER BY c.column_index), []) AS cols "
	                     "FROM duckdb_tables() t "
	                     "LEFT JOIN duckdb_columns() c "
	                     "  ON c.schema_name = t.schema_name AND c.table_name = t.table_name AND NOT c.internal "
	                     "WHERE t.schema_name = 'main' AND NOT t.internal "
	                     "GROUP BY ALL "
	                     "ORDER BY t.table_name");
	if (res->HasError()) {
		return GraphqlError("schema catalog query failed: " + res->GetError());
	}

	string mode;
	if (route_mode) {
		mode = options.mode.empty() ? "route" : options.mode;
	} else {
		mode = allowlist ? "allowlist" : "open";
	}

	string body = "{\"mode\":\"" + mode + "\"";
	if (route_mode && !options.route_name.empty()) {
		body += ",\"route\":\"" + QuackapiJsonEscape(options.route_name) + "\"";
	}
	body += ",\"tables\":{";
	bool first_table = true;
	while (true) {
		auto chunk = res->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		for (idx_t r = 0; r < chunk->size(); r++) {
			auto table = chunk->GetValue(0, r).ToString();
			if (route_mode || allowlist) {
				if (!TableInList(allowed, table)) {
					continue;
				}
			}
			if (!first_table) {
				body += ",";
			}
			first_table = false;
			body += "\"" + QuackapiJsonEscape(table) + "\":[";
			auto cols_val = chunk->GetValue(1, r);
			if (!cols_val.IsNull() && cols_val.type().id() == LogicalTypeId::LIST) {
				auto &children = ListValue::GetChildren(cols_val);
				for (idx_t c = 0; c < children.size(); c++) {
					if (c > 0) {
						body += ",";
					}
					if (!children[c].IsNull()) {
						body += "\"" + QuackapiJsonEscape(children[c].ToString()) + "\"";
					}
				}
			}
			body += "]";
		}
	}
	body += "},\"note\":\"" + QuackapiJsonEscape(GraphqlSchemaNote()) + "\"}";
	return body;
}

GraphqlDdlParserExtension::GraphqlDdlParserExtension() {
	parse_function = GraphqlDdlParse;
	plan_function = GraphqlDdlPlan;
}

//===--------------------------------------------------------------------===//
// quackapi_graphql_routes() — inspect named mounts
//===--------------------------------------------------------------------===//

struct GraphqlRoutesBindData : public TableFunctionData {};

struct GraphqlRoutesGlobalState : public GlobalTableFunctionState {
	vector<QuackapiGraphqlRoute> routes;
	idx_t offset = 0;
};

unique_ptr<FunctionData> GraphqlRoutesBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                           vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("method");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("path");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
	names.emplace_back("tables");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("require_auth");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("limit");
	return make_uniq<GraphqlRoutesBindData>();
}

unique_ptr<GlobalTableFunctionState> GraphqlRoutesInit(ClientContext &context, TableFunctionInitInput &) {
	auto state = make_uniq<GraphqlRoutesGlobalState>();
	state->routes = QuackapiState::Get(*context.db).SnapshotGraphqlRoutes();
	return std::move(state);
}

void GraphqlRoutesExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<GraphqlRoutesGlobalState>();
	idx_t row = 0;
	while (state.offset < state.routes.size() && row < STANDARD_VECTOR_SIZE) {
		auto &r = state.routes[state.offset];
		output.SetValue(0, row, Value(r.name));
		output.SetValue(1, row, Value(r.method));
		output.SetValue(2, row, Value(r.path));
		vector<Value> table_vals;
		for (auto &t : r.tables) {
			table_vals.emplace_back(t);
		}
		output.SetValue(3, row, Value::LIST(LogicalType::VARCHAR, table_vals));
		output.SetValue(4, row, r.require_auth.empty() ? Value() : Value(r.require_auth));
		output.SetValue(5, row, Value::BIGINT((int64_t)r.limit));
		row++;
		state.offset++;
	}
	output.SetCardinality(row);
}

void RegisterQuackapiGraphqlFunctions(ExtensionLoader &loader) {
	TableFunction tf("quackapi_graphql_tables", {}, GraphqlTablesExec, GraphqlTablesBind, GraphqlTablesInit);
	loader.RegisterFunction(tf);
	TableFunction routes("quackapi_graphql_routes", {}, GraphqlRoutesExec, GraphqlRoutesBind, GraphqlRoutesInit);
	loader.RegisterFunction(routes);
}

} // namespace duckdb
