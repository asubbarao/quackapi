#include "quackapi_policy.hpp"

#include <algorithm>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/tableref/list.hpp"

#include "quackapi_state.hpp"
#include "quackapi_util.hpp"

namespace duckdb {

namespace {

bool IsIdentStart(char c) {
	return StringUtil::CharacterIsAlpha(c) || c == '_';
}

bool IsIdentChar(char c) {
	return StringUtil::CharacterIsAlpha(c) || StringUtil::CharacterIsDigit(c) || c == '_';
}

//! First whitespace-delimited token (space/tab/newline all count as boundaries).
string NextToken(const string &s, idx_t &consumed) {
	idx_t i = 0;
	while (i < s.size() && !StringUtil::CharacterIsSpace(s[i])) {
		i++;
	}
	consumed = i;
	return s.substr(0, i);
}

//! Extract a parenthesized span starting at rest[0] == '('. Balanced parens;
//! ignores parens inside single-quoted strings ('' escape).
bool ExtractParenGroup(const string &rest, idx_t start, string &inner, idx_t &end_out) {
	if (start >= rest.size() || rest[start] != '(') {
		return false;
	}
	int depth = 0;
	bool in_str = false;
	for (idx_t i = start; i < rest.size(); i++) {
		char c = rest[i];
		if (in_str) {
			if (c == '\'') {
				if (i + 1 < rest.size() && rest[i + 1] == '\'') {
					i++; // escaped quote
				} else {
					in_str = false;
				}
			}
			continue;
		}
		if (c == '\'') {
			in_str = true;
			continue;
		}
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
			if (depth == 0) {
				inner = rest.substr(start + 1, i - start - 1);
				end_out = i + 1;
				return true;
			}
		}
	}
	return false;
}

//! Parse "col [TYPE], col [TYPE], ..." into names + optional types.
bool ParseColList(const string &inner, vector<string> &names, vector<string> &types, string &err) {
	names.clear();
	types.clear();
	string s = QuackapiTrim(inner);
	if (s.empty()) {
		err = "column list must not be empty";
		return false;
	}
	idx_t i = 0;
	while (i < s.size()) {
		while (i < s.size() && (StringUtil::CharacterIsSpace(s[i]) || s[i] == ',')) {
			i++;
		}
		if (i >= s.size()) {
			break;
		}
		if (!IsIdentStart(s[i])) {
			err = "expected column name in list";
			return false;
		}
		idx_t start = i;
		i++;
		while (i < s.size() && IsIdentChar(s[i])) {
			i++;
		}
		string col = s.substr(start, i - start);
		while (i < s.size() && StringUtil::CharacterIsSpace(s[i])) {
			i++;
		}
		string typ;
		if (i < s.size() && IsIdentStart(s[i])) {
			idx_t ts = i;
			i++;
			while (i < s.size() && IsIdentChar(s[i])) {
				i++;
			}
			typ = StringUtil::Upper(s.substr(ts, i - ts));
			if (typ == "INT") {
				typ = "INTEGER";
			} else if (typ == "BOOL") {
				typ = "BOOLEAN";
			} else if (typ == "TEXT" || typ == "STRING") {
				typ = "VARCHAR";
			} else if (typ == "REAL") {
				typ = "FLOAT";
			}
		}
		names.push_back(col);
		types.push_back(typ);
	}
	if (names.empty()) {
		err = "column list must not be empty";
		return false;
	}
	return true;
}

string JoinComma(const vector<string> &parts) {
	string out;
	for (idx_t i = 0; i < parts.size(); i++) {
		if (i > 0) {
			out += ", ";
		}
		out += parts[i];
	}
	return out;
}

//! Replace whole-word identifier `val` with `replacement` (masking policy body).
string SubstituteValPlaceholder(const string &expr, const string &replacement) {
	string out;
	out.reserve(expr.size() + 8);
	bool in_str = false;
	for (idx_t i = 0; i < expr.size();) {
		char c = expr[i];
		if (in_str) {
			out += c;
			if (c == '\'') {
				if (i + 1 < expr.size() && expr[i + 1] == '\'') {
					out += expr[i + 1];
					i += 2;
					continue;
				}
				in_str = false;
			}
			i++;
			continue;
		}
		if (c == '\'') {
			in_str = true;
			out += c;
			i++;
			continue;
		}
		if (IsIdentStart(c)) {
			idx_t j = i + 1;
			while (j < expr.size() && IsIdentChar(expr[j])) {
				j++;
			}
			string tok = expr.substr(i, j - i);
			if (StringUtil::Lower(tok) == "val") {
				out += replacement;
			} else {
				out += tok;
			}
			i = j;
			continue;
		}
		out += c;
		i++;
	}
	return out;
}

// `physical_relation` is built from parsed catalog/schema/table identifiers. It
// is deliberately not reconstructed from route text, so an attached catalog or
// quoted identifier cannot silently target a different object.
string BuildSecureSubqueryQuoted(const string &physical_relation, const QuackapiRowAccessPolicy *rap_for_where,
                                 const string &where_expr_or_empty,
                                 const vector<std::pair<string, string>> &masked_cols) {
	string where_sql;
	if (!where_expr_or_empty.empty()) {
		where_sql = " WHERE (" + where_expr_or_empty + ")";
	}
	(void)rap_for_where;
	if (masked_cols.empty()) {
		return "SELECT * FROM " + physical_relation + where_sql;
	}
	string excludes;
	string extras;
	for (idx_t i = 0; i < masked_cols.size(); i++) {
		if (i > 0) {
			excludes += ", ";
			extras += ", ";
		}
		// Quote masked column names in EXCLUDE / AS
		string col_esc;
		for (char c : masked_cols[i].first) {
			if (c == '"') {
				col_esc += "\"\"";
			} else {
				col_esc += c;
			}
		}
		string colq = "\"" + col_esc + "\"";
		excludes += colq;
		extras += "(" + masked_cols[i].second + ") AS " + colq;
	}
	return "SELECT * EXCLUDE (" + excludes + "), " + extras + " FROM " + physical_relation + where_sql;
}

//===--------------------------------------------------------------------===//
// DDL parse data
//===--------------------------------------------------------------------===//

struct PolicyDdlParseData : public ParserExtensionParseData {
	//! CREATE_ROW | CREATE_MASK | DROP_ROW | DROP_MASK | BIND_ROW | UNBIND_ROW | BIND_MASK | UNBIND_MASK
	string action;
	bool or_replace = false;
	string name;
	string value_type;          // masking ON <type>
	vector<string> arg_columns; // RAP signature / bind ON cols / mask column
	vector<string> arg_types;
	string expression;
	string table_name;
	string column_name; // masking bind

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto copy = make_uniq<PolicyDdlParseData>();
		copy->action = action;
		copy->or_replace = or_replace;
		copy->name = name;
		copy->value_type = value_type;
		copy->arg_columns = arg_columns;
		copy->arg_types = arg_types;
		copy->expression = expression;
		copy->table_name = table_name;
		copy->column_name = column_name;
		return std::move(copy);
	}
	string ToString() const override {
		return action + " POLICY " + name;
	}
};

ParserExtensionParseResult PolicyDdlParseText(const string &query) {
	auto q = QuackapiTrim(query);
	auto upper = StringUtil::Upper(q);

	// ---- DROP ROW ACCESS POLICY / DROP MASKING POLICY ----
	if (StringUtil::StartsWith(upper, "DROP ROW ACCESS POLICY ")) {
		auto name = QuackapiTrim(q.substr(23));
		if (name.empty() || name.find(' ') != string::npos) {
			return ParserExtensionParseResult("DROP ROW ACCESS POLICY expects a single policy name");
		}
		auto data = make_uniq<PolicyDdlParseData>();
		data->action = "DROP_ROW";
		data->name = name;
		return ParserExtensionParseResult(std::move(data));
	}
	if (StringUtil::StartsWith(upper, "DROP MASKING POLICY ")) {
		auto name = QuackapiTrim(q.substr(20));
		if (name.empty() || name.find(' ') != string::npos) {
			return ParserExtensionParseResult("DROP MASKING POLICY expects a single policy name");
		}
		auto data = make_uniq<PolicyDdlParseData>();
		data->action = "DROP_MASK";
		data->name = name;
		return ParserExtensionParseResult(std::move(data));
	}

	// ---- CREATE [OR REPLACE] ROW ACCESS POLICY ----
	bool or_replace = false;
	idx_t pos = 0;
	bool is_create_row = false;
	bool is_create_mask = false;
	if (StringUtil::StartsWith(upper, "CREATE OR REPLACE ROW ACCESS POLICY ")) {
		pos = 36;
		or_replace = true;
		is_create_row = true;
	} else if (StringUtil::StartsWith(upper, "CREATE ROW ACCESS POLICY ")) {
		pos = 25;
		is_create_row = true;
	} else if (StringUtil::StartsWith(upper, "CREATE OR REPLACE MASKING POLICY ")) {
		pos = 33;
		or_replace = true;
		is_create_mask = true;
	} else if (StringUtil::StartsWith(upper, "CREATE MASKING POLICY ")) {
		pos = 22;
		is_create_mask = true;
	}

	if (is_create_row) {
		auto rest = QuackapiTrim(q.substr(pos));
		idx_t name_len = 0;
		auto name = NextToken(rest, name_len);
		if (name.empty() || name_len >= rest.size()) {
			return ParserExtensionParseResult(
			    "CREATE ROW ACCESS POLICY <name> AS (<cols>) RETURNS BOOLEAN USING (<expr>)");
		}
		rest = QuackapiTrim(rest.substr(name_len));
		auto ru = StringUtil::Upper(rest);
		if (!StringUtil::StartsWith(ru, "AS")) {
			return ParserExtensionParseResult("Expected AS (<cols>) after policy name");
		}
		rest = QuackapiTrim(rest.substr(2));
		if (rest.empty() || rest[0] != '(') {
			return ParserExtensionParseResult("Expected AS (<cols>)");
		}
		string col_inner;
		idx_t after_cols = 0;
		if (!ExtractParenGroup(rest, 0, col_inner, after_cols)) {
			return ParserExtensionParseResult("Unterminated column list in AS (...)");
		}
		vector<string> cols, types;
		string err;
		if (!ParseColList(col_inner, cols, types, err)) {
			return ParserExtensionParseResult("ROW ACCESS POLICY AS (...): " + err);
		}
		rest = QuackapiTrim(rest.substr(after_cols));
		ru = StringUtil::Upper(rest);
		// RETURNS BOOLEAN
		if (!StringUtil::StartsWith(ru, "RETURNS")) {
			return ParserExtensionParseResult("Expected RETURNS BOOLEAN after AS (...)");
		}
		rest = QuackapiTrim(rest.substr(7));
		ru = StringUtil::Upper(rest);
		if (!StringUtil::StartsWith(ru, "BOOLEAN")) {
			return ParserExtensionParseResult("ROW ACCESS POLICY must RETURNS BOOLEAN");
		}
		rest = QuackapiTrim(rest.substr(7));
		ru = StringUtil::Upper(rest);
		if (!StringUtil::StartsWith(ru, "USING")) {
			return ParserExtensionParseResult("Expected USING (<expr>) after RETURNS BOOLEAN");
		}
		rest = QuackapiTrim(rest.substr(5));
		if (rest.empty() || rest[0] != '(') {
			return ParserExtensionParseResult("Expected USING (<expr>)");
		}
		string expr;
		idx_t after_expr = 0;
		if (!ExtractParenGroup(rest, 0, expr, after_expr)) {
			return ParserExtensionParseResult("Unterminated USING (...) expression");
		}
		expr = QuackapiTrim(expr);
		if (expr.empty()) {
			return ParserExtensionParseResult("USING expression must not be empty");
		}
		rest = QuackapiTrim(rest.substr(after_expr));
		if (!rest.empty()) {
			return ParserExtensionParseResult("Unexpected tokens after USING (...)");
		}
		auto data = make_uniq<PolicyDdlParseData>();
		data->action = "CREATE_ROW";
		data->or_replace = or_replace;
		data->name = name;
		data->arg_columns = std::move(cols);
		data->arg_types = std::move(types);
		data->expression = expr;
		return ParserExtensionParseResult(std::move(data));
	}

	if (is_create_mask) {
		auto rest = QuackapiTrim(q.substr(pos));
		idx_t name_len = 0;
		auto name = NextToken(rest, name_len);
		if (name.empty() || name_len >= rest.size()) {
			return ParserExtensionParseResult("CREATE MASKING POLICY <name> ON <type> USING (<expr>)");
		}
		rest = QuackapiTrim(rest.substr(name_len));
		auto ru = StringUtil::Upper(rest);
		if (!StringUtil::StartsWith(ru, "ON")) {
			return ParserExtensionParseResult("Expected ON <type> after masking policy name");
		}
		// ON may be followed by space or newline
		if (rest.size() == 2 || !StringUtil::CharacterIsSpace(rest[2])) {
			// "ON" alone without trailing space after token — still need type next
			if (rest.size() == 2) {
				return ParserExtensionParseResult("Expected ON <type> USING (<expr>)");
			}
		}
		rest = QuackapiTrim(rest.substr(2));
		idx_t type_len = 0;
		auto vtype = StringUtil::Upper(NextToken(rest, type_len));
		if (vtype.empty() || type_len >= rest.size()) {
			return ParserExtensionParseResult("Expected ON <type> USING (<expr>)");
		}
		if (vtype == "INT") {
			vtype = "INTEGER";
		} else if (vtype == "BOOL") {
			vtype = "BOOLEAN";
		} else if (vtype == "TEXT" || vtype == "STRING") {
			vtype = "VARCHAR";
		} else if (vtype == "REAL") {
			vtype = "FLOAT";
		}
		rest = QuackapiTrim(rest.substr(type_len));
		ru = StringUtil::Upper(rest);
		if (!StringUtil::StartsWith(ru, "USING")) {
			return ParserExtensionParseResult("Expected USING (<expr>) after ON <type>");
		}
		rest = QuackapiTrim(rest.substr(5));
		if (rest.empty() || rest[0] != '(') {
			return ParserExtensionParseResult("Expected USING (<expr>)");
		}
		string expr;
		idx_t after_expr = 0;
		if (!ExtractParenGroup(rest, 0, expr, after_expr)) {
			return ParserExtensionParseResult("Unterminated USING (...) expression");
		}
		expr = QuackapiTrim(expr);
		if (expr.empty()) {
			return ParserExtensionParseResult("USING expression must not be empty");
		}
		rest = QuackapiTrim(rest.substr(after_expr));
		if (!rest.empty()) {
			return ParserExtensionParseResult("Unexpected tokens after USING (...)");
		}
		auto data = make_uniq<PolicyDdlParseData>();
		data->action = "CREATE_MASK";
		data->or_replace = or_replace;
		data->name = name;
		data->value_type = vtype;
		data->expression = expr;
		return ParserExtensionParseResult(std::move(data));
	}

	// ---- ALTER TABLE … ----
	if (StringUtil::StartsWith(upper, "ALTER TABLE ")) {
		auto rest = QuackapiTrim(q.substr(12));
		// table name
		string table;
		if (rest.empty()) {
			return ParserExtensionParseResult();
		}
		if (rest[0] == '"') {
			// quoted
			idx_t i = 1;
			string result;
			while (i < rest.size()) {
				if (rest[i] == '"') {
					if (i + 1 < rest.size() && rest[i + 1] == '"') {
						result += '"';
						i += 2;
						continue;
					}
					table = result;
					rest = QuackapiTrim(rest.substr(i + 1));
					break;
				}
				result += rest[i];
				i++;
			}
			if (table.empty()) {
				return ParserExtensionParseResult();
			}
		} else {
			idx_t i = 0;
			while (i < rest.size() && IsIdentChar(rest[i])) {
				i++;
			}
			if (i == 0) {
				return ParserExtensionParseResult();
			}
			table = rest.substr(0, i);
			rest = QuackapiTrim(rest.substr(i));
		}
		auto ru = StringUtil::Upper(rest);

		// ADD [OR REPLACE] ROW ACCESS POLICY <p> ON (<cols>)
		bool add_or_replace = false;
		if (StringUtil::StartsWith(ru, "ADD OR REPLACE ROW ACCESS POLICY ")) {
			rest = QuackapiTrim(rest.substr(33));
			add_or_replace = true;
		} else if (StringUtil::StartsWith(ru, "ADD ROW ACCESS POLICY ")) {
			rest = QuackapiTrim(rest.substr(22));
		} else if (StringUtil::StartsWith(ru, "DROP ROW ACCESS POLICY ")) {
			auto pname = QuackapiTrim(rest.substr(23));
			if (pname.empty() || pname.find(' ') != string::npos) {
				return ParserExtensionParseResult("ALTER TABLE … DROP ROW ACCESS POLICY expects a policy name");
			}
			auto data = make_uniq<PolicyDdlParseData>();
			data->action = "UNBIND_ROW";
			data->table_name = table;
			data->name = pname;
			return ParserExtensionParseResult(std::move(data));
		} else if (StringUtil::StartsWith(ru, "MODIFY COLUMN ") || StringUtil::StartsWith(ru, "ALTER COLUMN ")) {
			idx_t skip = StringUtil::StartsWith(ru, "MODIFY COLUMN ") ? 14 : 13;
			rest = QuackapiTrim(rest.substr(skip));
			// column name
			string col;
			idx_t ci = 0;
			if (rest.empty()) {
				return ParserExtensionParseResult("ALTER TABLE … COLUMN expects a column name");
			}
			if (rest[0] == '"') {
				return ParserExtensionParseResult("Quoted column names in SET MASKING POLICY not yet supported");
			}
			while (ci < rest.size() && IsIdentChar(rest[ci])) {
				ci++;
			}
			col = rest.substr(0, ci);
			rest = QuackapiTrim(rest.substr(ci));
			ru = StringUtil::Upper(rest);
			if (StringUtil::StartsWith(ru, "SET MASKING POLICY ")) {
				auto pname = QuackapiTrim(rest.substr(19));
				if (pname.empty() || pname.find(' ') != string::npos) {
					return ParserExtensionParseResult("SET MASKING POLICY expects a policy name");
				}
				auto data = make_uniq<PolicyDdlParseData>();
				data->action = "BIND_MASK";
				data->or_replace = true; // SET replaces
				data->table_name = table;
				data->column_name = col;
				data->name = pname;
				return ParserExtensionParseResult(std::move(data));
			}
			if (StringUtil::StartsWith(ru, "UNSET MASKING POLICY") ||
			    StringUtil::StartsWith(ru, "DROP MASKING POLICY")) {
				auto data = make_uniq<PolicyDdlParseData>();
				data->action = "UNBIND_MASK";
				data->table_name = table;
				data->column_name = col;
				return ParserExtensionParseResult(std::move(data));
			}
			// Not our ALTER COLUMN form — leave to core.
			return ParserExtensionParseResult();
		} else {
			// Not a policy ALTER — leave to DuckDB core.
			return ParserExtensionParseResult();
		}

		// ADD ROW ACCESS POLICY path continues
		idx_t pname_len = 0;
		string pname = NextToken(rest, pname_len);
		if (pname.empty()) {
			return ParserExtensionParseResult("ADD ROW ACCESS POLICY expects a policy name");
		}
		rest = pname_len >= rest.size() ? string() : QuackapiTrim(rest.substr(pname_len));
		ru = StringUtil::Upper(rest);
		if (!StringUtil::StartsWith(ru, "ON")) {
			return ParserExtensionParseResult("Expected ON (<cols>) after policy name");
		}
		rest = QuackapiTrim(rest.substr(2));
		if (rest.empty() || rest[0] != '(') {
			return ParserExtensionParseResult("Expected ON (<cols>)");
		}
		string col_inner;
		idx_t after = 0;
		if (!ExtractParenGroup(rest, 0, col_inner, after)) {
			return ParserExtensionParseResult("Unterminated ON (...) column list");
		}
		vector<string> cols, types;
		string err;
		if (!ParseColList(col_inner, cols, types, err)) {
			return ParserExtensionParseResult("ON (...): " + err);
		}
		rest = QuackapiTrim(rest.substr(after));
		if (!rest.empty()) {
			return ParserExtensionParseResult("Unexpected tokens after ON (...)");
		}
		auto data = make_uniq<PolicyDdlParseData>();
		data->action = "BIND_ROW";
		data->or_replace = add_or_replace;
		data->table_name = table;
		data->name = pname;
		data->arg_columns = std::move(cols);
		return ParserExtensionParseResult(std::move(data));
	}

	return ParserExtensionParseResult();
}

ParserExtensionParseResult PolicyDdlParse(ParserExtensionInfo *, const vector<SimpleToken> &tokens) {
	auto statement = QuackapiStatementFromTokens(tokens);
	return QuackapiClaimTokens(PolicyDdlParseText(statement.query), statement.consumed_tokens);
}

//===--------------------------------------------------------------------===//
// Apply table function
//===--------------------------------------------------------------------===//

struct ApplyPolicyBindData : public TableFunctionData {
	string action;
	bool or_replace = false;
	string name;
	string value_type;
	string arg_columns_csv; // comma-separated
	string arg_types_csv;
	string expression;
	string table_name;
	string column_name;
	bool finished = false;
};

vector<string> SplitCsv(const string &csv) {
	vector<string> out;
	if (csv.empty()) {
		return out;
	}
	idx_t i = 0;
	while (i < csv.size()) {
		idx_t j = i;
		while (j < csv.size() && csv[j] != ',') {
			j++;
		}
		out.push_back(QuackapiTrim(csv.substr(i, j - i)));
		i = j < csv.size() ? j + 1 : j;
	}
	return out;
}

unique_ptr<FunctionData> ApplyPolicyBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto bind_data = make_uniq<ApplyPolicyBindData>();
	bind_data->action = input.inputs[0].GetValue<string>();
	bind_data->or_replace = input.inputs[1].GetValue<bool>();
	bind_data->name = input.inputs[2].GetValue<string>();
	bind_data->value_type = input.inputs[3].GetValue<string>();
	bind_data->arg_columns_csv = input.inputs[4].GetValue<string>();
	bind_data->arg_types_csv = input.inputs[5].GetValue<string>();
	bind_data->expression = input.inputs[6].GetValue<string>();
	bind_data->table_name = input.inputs[7].GetValue<string>();
	bind_data->column_name = input.inputs[8].GetValue<string>();
	BindStatusColumn(return_types, names);
	return std::move(bind_data);
}

void ApplyPolicyExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ApplyPolicyBindData>();
	if (bind_data.finished) {
		return;
	}
	auto &state = QuackapiState::Get(*context.db);
	string message;
	auto cols = SplitCsv(bind_data.arg_columns_csv);
	auto types = SplitCsv(bind_data.arg_types_csv);

	if (bind_data.action == "CREATE_ROW") {
		QuackapiRowAccessPolicy p;
		p.name = bind_data.name;
		p.arg_columns = cols;
		p.arg_types = types;
		while (p.arg_types.size() < p.arg_columns.size()) {
			p.arg_types.push_back("");
		}
		p.expression = bind_data.expression;
		state.AddRowAccessPolicy(p, bind_data.or_replace);
		message = StringUtil::Format("Row access policy %s", p.name);
	} else if (bind_data.action == "CREATE_MASK") {
		QuackapiMaskingPolicy p;
		p.name = bind_data.name;
		p.value_type = bind_data.value_type;
		p.expression = bind_data.expression;
		state.AddMaskingPolicy(p, bind_data.or_replace);
		message = StringUtil::Format("Masking policy %s ON %s", p.name, p.value_type);
	} else if (bind_data.action == "DROP_ROW") {
		if (!state.DropRowAccessPolicy(bind_data.name)) {
			throw InvalidInputException("Row access policy \"%s\" does not exist", bind_data.name);
		}
		message = StringUtil::Format("Dropped row access policy %s", bind_data.name);
	} else if (bind_data.action == "DROP_MASK") {
		if (!state.DropMaskingPolicy(bind_data.name)) {
			throw InvalidInputException("Masking policy \"%s\" does not exist", bind_data.name);
		}
		message = StringUtil::Format("Dropped masking policy %s", bind_data.name);
	} else if (bind_data.action == "BIND_ROW") {
		QuackapiRowAccessBinding b;
		b.table_name = bind_data.table_name;
		b.policy_name = bind_data.name;
		b.columns = cols;
		state.BindRowAccessPolicy(b, bind_data.or_replace);
		message = StringUtil::Format("Bound row access policy %s on %s", b.policy_name, b.table_name);
	} else if (bind_data.action == "UNBIND_ROW") {
		if (!state.UnbindRowAccessPolicy(bind_data.table_name, bind_data.name)) {
			throw InvalidInputException("Table \"%s\" has no row access policy \"%s\"", bind_data.table_name,
			                            bind_data.name);
		}
		message = StringUtil::Format("Dropped row access policy %s from %s", bind_data.name, bind_data.table_name);
	} else if (bind_data.action == "BIND_MASK") {
		QuackapiMaskingBinding b;
		b.table_name = bind_data.table_name;
		b.column_name = bind_data.column_name;
		b.policy_name = bind_data.name;
		state.BindMaskingPolicy(b, bind_data.or_replace);
		message = StringUtil::Format("Set masking policy %s on %s.%s", b.policy_name, b.table_name, b.column_name);
	} else if (bind_data.action == "UNBIND_MASK") {
		if (!state.UnbindMaskingPolicy(bind_data.table_name, bind_data.column_name)) {
			throw InvalidInputException("Column \"%s\".\"%s\" has no masking policy", bind_data.table_name,
			                            bind_data.column_name);
		}
		message = StringUtil::Format("Unset masking policy on %s.%s", bind_data.table_name, bind_data.column_name);
	} else {
		throw InvalidInputException("Unknown policy action \"%s\"", bind_data.action);
	}

	EmitOneShotStatus(output, bind_data.finished, message);
}

TableFunction MakeApplyPolicyFunction() {
	// action, or_replace, name, value_type, arg_columns_csv, arg_types_csv, expression, table, column
	return MakeApplyDdlFunction("quackapi_apply_policy",
	                            {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                             LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                             LogicalType::VARCHAR},
	                            ApplyPolicyExec, ApplyPolicyBind);
}

ParserExtensionPlanResult PolicyDdlPlan(ParserExtensionInfo *, ClientContext &,
                                        unique_ptr<ParserExtensionParseData> parse_data) {
	auto &data = static_cast<PolicyDdlParseData &>(*parse_data);
	ParserExtensionPlanResult result;
	result.function = MakeApplyPolicyFunction();
	result.parameters.push_back(Value(data.action));
	result.parameters.push_back(Value::BOOLEAN(data.or_replace));
	result.parameters.push_back(Value(data.name));
	result.parameters.push_back(Value(data.value_type));
	result.parameters.push_back(Value(JoinComma(data.arg_columns)));
	result.parameters.push_back(Value(JoinComma(data.arg_types)));
	result.parameters.push_back(Value(data.expression));
	result.parameters.push_back(Value(data.table_name));
	result.parameters.push_back(Value(data.column_name));
	FinishDdlPlan(result);
	return result;
}

//===--------------------------------------------------------------------===//
// quackapi_policies()
//===--------------------------------------------------------------------===//

struct PolicyRow {
	string name;
	string kind;
	string signature;
	string expression;
	string bound_table;
	string bound_columns;
};

struct PoliciesBindData : public TableFunctionData {};

struct PoliciesGlobalState : public GlobalTableFunctionState {
	vector<PolicyRow> rows;
	idx_t offset = 0;
};

unique_ptr<FunctionData> PoliciesBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                      vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("kind");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("signature");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("expression");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("bound_table");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("bound_columns");
	return make_uniq<PoliciesBindData>();
}

unique_ptr<GlobalTableFunctionState> PoliciesInit(ClientContext &context, TableFunctionInitInput &) {
	auto st = make_uniq<PoliciesGlobalState>();
	auto &state = QuackapiState::Get(*context.db);
	auto raps = state.SnapshotRowAccessPolicies();
	auto masks = state.SnapshotMaskingPolicies();
	auto rap_binds = state.SnapshotRowAccessBindings();
	auto mask_binds = state.SnapshotMaskingBindings();

	for (auto &p : raps) {
		string sig = "(";
		for (idx_t i = 0; i < p.arg_columns.size(); i++) {
			if (i > 0) {
				sig += ", ";
			}
			sig += p.arg_columns[i];
			if (i < p.arg_types.size() && !p.arg_types[i].empty()) {
				sig += " ";
				sig += p.arg_types[i];
			}
		}
		sig += ") RETURNS BOOLEAN";
		bool any = false;
		for (auto &b : rap_binds) {
			if (b.policy_name == p.name) {
				PolicyRow row;
				row.name = p.name;
				row.kind = "ROW_ACCESS";
				row.signature = sig;
				row.expression = p.expression;
				row.bound_table = b.table_name;
				row.bound_columns = JoinComma(b.columns);
				st->rows.push_back(std::move(row));
				any = true;
			}
		}
		if (!any) {
			PolicyRow row;
			row.name = p.name;
			row.kind = "ROW_ACCESS";
			row.signature = sig;
			row.expression = p.expression;
			st->rows.push_back(std::move(row));
		}
	}
	for (auto &p : masks) {
		string sig = "ON " + p.value_type;
		bool any = false;
		for (auto &b : mask_binds) {
			if (b.policy_name == p.name) {
				PolicyRow row;
				row.name = p.name;
				row.kind = "MASKING";
				row.signature = sig;
				row.expression = p.expression;
				row.bound_table = b.table_name;
				row.bound_columns = b.column_name;
				st->rows.push_back(std::move(row));
				any = true;
			}
		}
		if (!any) {
			PolicyRow row;
			row.name = p.name;
			row.kind = "MASKING";
			row.signature = sig;
			row.expression = p.expression;
			st->rows.push_back(std::move(row));
		}
	}
	return std::move(st);
}

void PoliciesExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<PoliciesGlobalState>();
	idx_t row = 0;
	while (state.offset < state.rows.size() && row < STANDARD_VECTOR_SIZE) {
		auto &r = state.rows[state.offset];
		output.SetValue(0, row, Value(r.name));
		output.SetValue(1, row, Value(r.kind));
		output.SetValue(2, row, Value(r.signature));
		output.SetValue(3, row, Value(r.expression));
		output.SetValue(4, row, Value(r.bound_table));
		output.SetValue(5, row, Value(r.bound_columns));
		row++;
		state.offset++;
	}
	output.SetCardinality(row);
}

} // namespace

//===--------------------------------------------------------------------===//
// Enforcement rewrite
//===--------------------------------------------------------------------===//

//! Parsed identity of a catalog relation. Policy DDL historically stores a
//! string name, so turn that name into the same AST representation used for a
//! route before comparing it to a route reference.
struct PolicyTableIdentity {
	string catalog;
	string schema;
	string table;
};

string QuoteCatalogIdentifier(const string &ident) {
	string out = "\"";
	for (auto c : ident) {
		if (c == '"') {
			out += "\"\"";
		} else {
			out += c;
		}
	}
	return out + "\"";
}

bool SameCatalogIdentifier(const string &left, const string &right) {
	return StringUtil::Lower(left) == StringUtil::Lower(right);
}

string CurrentCatalog(Connection &con) {
	auto result = con.Query("SELECT current_database()");
	if (result->HasError()) {
		return string();
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0 || chunk->GetValue(0, 0).IsNull()) {
		return string();
	}
	return chunk->GetValue(0, 0).ToString();
}

PolicyTableIdentity IdentityFromBaseRef(const BaseTableRef &ref, const string &default_catalog) {
	PolicyTableIdentity identity;
	auto &qualified = ref.GetQualifiedName();
	identity.catalog = qualified.Catalog().empty() ? default_catalog : qualified.Catalog().GetIdentifierName();
	identity.schema = qualified.Schema().empty() ? "main" : qualified.Schema().GetIdentifierName();
	identity.table = qualified.Name().GetIdentifierName();
	return identity;
}

bool ParsePolicyTableIdentity(const string &table_name, const string &default_catalog, PolicyTableIdentity &identity) {
	try {
		Parser parser;
		parser.ParseQuery("SELECT * FROM " + table_name);
		if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
			return false;
		}
		auto &select = parser.statements[0]->Cast<SelectStatement>();
		if (!select.node || select.node->type != QueryNodeType::SELECT_NODE) {
			return false;
		}
		auto &node = select.node->Cast<SelectNode>();
		if (!node.from_table || node.from_table->type != TableReferenceType::BASE_TABLE) {
			return false;
		}
		identity = IdentityFromBaseRef(node.from_table->Cast<BaseTableRef>(), default_catalog);
		return !identity.table.empty();
	} catch (...) {
		return false;
	}
}

bool SamePolicyTableIdentity(const PolicyTableIdentity &left, const PolicyTableIdentity &right) {
	return SameCatalogIdentifier(left.catalog, right.catalog) && SameCatalogIdentifier(left.schema, right.schema) &&
	       SameCatalogIdentifier(left.table, right.table);
}

string QuotePolicyTableIdentity(const PolicyTableIdentity &identity) {
	return QuoteCatalogIdentifier(identity.catalog) + "." + QuoteCatalogIdentifier(identity.schema) + "." +
	       QuoteCatalogIdentifier(identity.table);
}

//! Replace a policy signature argument without touching SQL string literals.
string ReplacePolicyIdentifier(const string &expr, const string &from, const string &to) {
	string out;
	bool in_str = false;
	for (idx_t i = 0; i < expr.size();) {
		if (in_str) {
			out += expr[i];
			if (expr[i] == '\'') {
				if (i + 1 < expr.size() && expr[i + 1] == '\'') {
					out += expr[i + 1];
					i += 2;
					continue;
				}
				in_str = false;
			}
			i++;
			continue;
		}
		if (expr[i] == '\'') {
			in_str = true;
			out += expr[i++];
			continue;
		}
		if (IsIdentStart(expr[i])) {
			idx_t end = i + 1;
			while (end < expr.size() && IsIdentChar(expr[end])) {
				end++;
			}
			auto token = expr.substr(i, end - i);
			out += SameCatalogIdentifier(token, from) ? to : token;
			i = end;
			continue;
		}
		out += expr[i++];
	}
	return out;
}

struct PolicyRewriteContext {
	DatabaseInstance &db;
	Connection con;
	const vector<QuackapiRowAccessBinding> &rap_bindings;
	const vector<QuackapiMaskingBinding> &mask_bindings;
	bool authenticated;
	string default_catalog;
	bool deny_unauthenticated = false;
	//! Report bound relations instead of rewriting them. Set for handlers the
	//! rewriter cannot express, where the answer is admit-or-refuse.
	bool detect_only = false;
	string error;
	unordered_map<string, bool> view_cache;

	PolicyRewriteContext(DatabaseInstance &db_p, const vector<QuackapiRowAccessBinding> &rap_bindings_p,
	                     const vector<QuackapiMaskingBinding> &mask_bindings_p, bool authenticated_p)
	    : db(db_p), con(db_p), rap_bindings(rap_bindings_p), mask_bindings(mask_bindings_p),
	      authenticated(authenticated_p), default_catalog(CurrentCatalog(con)) {
	}

	void Reject(const string &reason) {
		if (error.empty()) {
			error = reason;
		}
		if (!authenticated && !detect_only) {
			deny_unauthenticated = true;
		}
	}
};

bool IsCatalogView(PolicyRewriteContext &ctx, const PolicyTableIdentity &identity, bool &is_view) {
	auto key = StringUtil::Lower(identity.catalog) + "\x1f" + StringUtil::Lower(identity.schema) + "\x1f" +
	           StringUtil::Lower(identity.table);
	auto cached = ctx.view_cache.find(key);
	if (cached != ctx.view_cache.end()) {
		is_view = cached->second;
		return true;
	}
	auto result = ctx.con.Query(
	    "SELECT 1 FROM duckdb_views() WHERE database_name = ? AND schema_name = ? AND view_name = ? LIMIT 1",
	    Value(identity.catalog), Value(identity.schema), Value(identity.table));
	if (result->HasError()) {
		return false;
	}
	auto chunk = result->Fetch();
	is_view = chunk && chunk->size() > 0;
	ctx.view_cache.emplace(std::move(key), is_view);
	return true;
}

bool ParseSecureSubquery(const string &sql, unique_ptr<SelectStatement> &out) {
	try {
		Parser parser;
		parser.ParseQuery(sql);
		if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
			return false;
		}
		out.reset(static_cast<SelectStatement *>(parser.statements[0].release()));
		return true;
	} catch (...) {
		return false;
	}
}

bool RewritePolicyQueryNode(QueryNode &node, PolicyRewriteContext &ctx);
bool RewritePolicySelect(SelectStatement &statement, PolicyRewriteContext &ctx);

//! Policy-bearing relations can occur in scalar/EXISTS/IN subqueries inside a
//! SELECT list, predicate, join condition, or modifier — not only in FROM.
//! Walk parsed expressions recursively so those reads receive the same AST
//! rewrite as top-level relation sources.
bool RewritePolicyExpression(ParsedExpression &expression, PolicyRewriteContext &ctx) {
	if (!ctx.error.empty()) {
		return false;
	}
	try {
		if (expression.GetExpressionClass() == ExpressionClass::SUBQUERY) {
			auto &subquery = expression.Cast<SubqueryExpression>();
			if (!subquery.Subquery() || !RewritePolicySelect(*subquery.SubqueryMutable(), ctx)) {
				if (ctx.error.empty()) {
					ctx.Reject("policy enforcement could not inspect a nested subquery");
				}
				return false;
			}
		}
		bool rewritten = true;
		ParsedExpressionIterator::EnumerateChildren(expression, [&](ParsedExpression &child) {
			if (rewritten && !RewritePolicyExpression(child, ctx)) {
				rewritten = false;
			}
		});
		return rewritten && ctx.error.empty();
	} catch (...) {
		ctx.Reject("policy enforcement could not inspect a nested expression");
		return false;
	}
}

bool RewritePolicyModifiers(QueryNode &node, PolicyRewriteContext &ctx) {
	bool rewritten = true;
	try {
		ParsedExpressionIterator::EnumerateQueryNodeModifiers(node, [&](unique_ptr<ParsedExpression> &expression) {
			if (rewritten && expression && !RewritePolicyExpression(*expression, ctx)) {
				rewritten = false;
			}
		});
	} catch (...) {
		ctx.Reject("policy enforcement could not inspect a query modifier");
		return false;
	}
	return rewritten && ctx.error.empty();
}

bool RewritePolicySelect(SelectStatement &statement, PolicyRewriteContext &ctx) {
	if (!statement.node) {
		ctx.Reject("policy enforcement could not inspect an empty SELECT statement");
		return false;
	}
	return RewritePolicyQueryNode(*statement.node, ctx);
}

bool RewritePolicyTableRef(unique_ptr<TableRef> &ref, PolicyRewriteContext &ctx) {
	if (!ref || !ctx.error.empty()) {
		return false;
	}
	switch (ref->type) {
	case TableReferenceType::BASE_TABLE: {
		auto &base = ref->Cast<BaseTableRef>();
		auto identity = IdentityFromBaseRef(base, ctx.default_catalog);
		bool is_view = false;
		if (!IsCatalogView(ctx, identity, is_view)) {
			ctx.Reject("policy enforcement could not resolve catalog object identity");
			return false;
		}
		if (is_view) {
			// A view can hide a protected relation. Do not infer dependencies from
			// view SQL text: reject the indirect source until it has a bound rewrite.
			ctx.Reject("policy enforcement rejects indirect view reads");
			return false;
		}

		const QuackapiRowAccessBinding *rap_binding = nullptr;
		const QuackapiMaskingBinding *first_mask = nullptr;
		for (auto &binding : ctx.rap_bindings) {
			PolicyTableIdentity bound;
			if (!ParsePolicyTableIdentity(binding.table_name, ctx.default_catalog, bound)) {
				ctx.Reject("policy binding has an invalid catalog identity");
				return false;
			}
			if (SamePolicyTableIdentity(identity, bound)) {
				rap_binding = &binding;
				break;
			}
		}
		for (auto &binding : ctx.mask_bindings) {
			PolicyTableIdentity bound;
			if (!ParsePolicyTableIdentity(binding.table_name, ctx.default_catalog, bound)) {
				ctx.Reject("policy binding has an invalid catalog identity");
				return false;
			}
			if (SamePolicyTableIdentity(identity, bound)) {
				first_mask = &binding;
				break;
			}
		}
		if (!rap_binding && !first_mask) {
			return true;
		}
		if (ctx.detect_only) {
			// Policies rewrite reads; there is no write policy to apply. Name the
			// relation so the refusal points at the binding that caused it.
			ctx.Reject(StringUtil::Format("policy enforcement supports one SELECT statement per handler, and this "
			                              "handler reaches policy-protected relation %s",
			                              QuotePolicyTableIdentity(identity)));
			return false;
		}
		if (!ctx.authenticated) {
			ctx.deny_unauthenticated = true;
			return false;
		}

		auto &state = QuackapiState::Get(ctx.db);
		QuackapiRowAccessPolicy rap_storage;
		const QuackapiRowAccessPolicy *rap = nullptr;
		string where_expr;
		if (rap_binding) {
			if (!state.GetRowAccessPolicy(rap_binding->policy_name, rap_storage)) {
				ctx.Reject("row-access policy binding references a missing policy");
				return false;
			}
			where_expr = rap_storage.expression;
			for (idx_t i = 0; i < rap_storage.arg_columns.size() && i < rap_binding->columns.size(); i++) {
				if (!SameCatalogIdentifier(rap_storage.arg_columns[i], rap_binding->columns[i])) {
					where_expr =
					    ReplacePolicyIdentifier(where_expr, rap_storage.arg_columns[i], rap_binding->columns[i]);
				}
			}
			rap = &rap_storage;
		}

		vector<std::pair<string, string>> masked;
		for (auto &binding : ctx.mask_bindings) {
			PolicyTableIdentity bound;
			if (!ParsePolicyTableIdentity(binding.table_name, ctx.default_catalog, bound)) {
				ctx.Reject("policy binding has an invalid catalog identity");
				return false;
			}
			if (!SamePolicyTableIdentity(identity, bound)) {
				continue;
			}
			QuackapiMaskingPolicy mask;
			if (!state.GetMaskingPolicy(binding.policy_name, mask)) {
				ctx.Reject("masking policy binding references a missing policy");
				return false;
			}
			masked.emplace_back(binding.column_name,
			                    SubstituteValPlaceholder(mask.expression, QuoteCatalogIdentifier(binding.column_name)));
		}

		unique_ptr<SelectStatement> secure_query;
		if (!ParseSecureSubquery(BuildSecureSubqueryQuoted(QuotePolicyTableIdentity(identity), rap, where_expr, masked),
		                         secure_query)) {
			ctx.Reject("policy enforcement could not construct a secure subquery");
			return false;
		}
		auto alias = ref->alias.empty() ? base.GetQualifiedName().Name() : ref->alias;
		auto replacement = make_uniq<SubqueryRef>(std::move(secure_query), alias);
		ref->CopyProperties(*replacement);
		replacement->alias = alias;
		ref = std::move(replacement);
		return true;
	}
	case TableReferenceType::SUBQUERY:
		return RewritePolicySelect(*ref->Cast<SubqueryRef>().subquery, ctx);
	case TableReferenceType::JOIN: {
		auto &join = ref->Cast<JoinRef>();
		if (!RewritePolicyTableRef(join.left, ctx) || !RewritePolicyTableRef(join.right, ctx)) {
			return false;
		}
		return !join.condition || RewritePolicyExpression(*join.condition, ctx);
	}
	case TableReferenceType::PIVOT: {
		auto &pivot = ref->Cast<PivotRef>();
		if (!RewritePolicyTableRef(pivot.source, ctx)) {
			return false;
		}
		for (auto &aggregate : pivot.aggregates) {
			if (aggregate && !RewritePolicyExpression(*aggregate, ctx)) {
				return false;
			}
		}
		return true;
	}
	case TableReferenceType::TABLE_FUNCTION:
		ctx.Reject("policy enforcement rejects table functions and table macros");
		return false;
	case TableReferenceType::CTE:
	case TableReferenceType::EMPTY_FROM:
		return true;
	case TableReferenceType::EXPRESSION_LIST: {
		auto &values = ref->Cast<ExpressionListRef>();
		for (auto &row : values.values) {
			for (auto &expression : row) {
				if (expression && !RewritePolicyExpression(*expression, ctx)) {
					return false;
				}
			}
		}
		return true;
	}
	default:
		ctx.Reject("policy enforcement rejects an unsupported relation source");
		return false;
	}
}

bool RewritePolicyQueryNode(QueryNode &node, PolicyRewriteContext &ctx) {
	for (auto &entry : node.cte_map.map) {
		if (!entry.second || !entry.second->query_node || !RewritePolicyQueryNode(*entry.second->query_node, ctx)) {
			ctx.Reject("policy enforcement could not inspect a common table expression");
			return false;
		}
	}
	switch (node.type) {
	case QueryNodeType::SELECT_NODE: {
		auto &select = node.Cast<SelectNode>();
		if (select.from_table && !RewritePolicyTableRef(select.from_table, ctx)) {
			return false;
		}
		for (auto &expression : select.select_list) {
			if (expression && !RewritePolicyExpression(*expression, ctx)) {
				return false;
			}
		}
		for (auto &expression : select.groups.group_expressions) {
			if (expression && !RewritePolicyExpression(*expression, ctx)) {
				return false;
			}
		}
		if ((select.where_clause && !RewritePolicyExpression(*select.where_clause, ctx)) ||
		    (select.having && !RewritePolicyExpression(*select.having, ctx)) ||
		    (select.qualify && !RewritePolicyExpression(*select.qualify, ctx))) {
			return false;
		}
		return RewritePolicyModifiers(node, ctx);
	}
	case QueryNodeType::SET_OPERATION_NODE: {
		auto &setop = node.Cast<SetOperationNode>();
		for (auto &child : setop.children) {
			if (!child || !RewritePolicyQueryNode(*child, ctx)) {
				return false;
			}
		}
		return RewritePolicyModifiers(node, ctx);
	}
	default:
		ctx.Reject("policy enforcement rejects an unsupported query form");
		return false;
	}
}

bool InspectPolicyExpressionList(vector<unique_ptr<ParsedExpression>> &expressions, PolicyRewriteContext &ctx) {
	for (auto &expression : expressions) {
		if (expression && !RewritePolicyExpression(*expression, ctx)) {
			return false;
		}
	}
	return true;
}

bool InspectPolicyCteMap(CommonTableExpressionMap &cte_map, PolicyRewriteContext &ctx) {
	for (auto &entry : cte_map.map) {
		if (!entry.second || !entry.second->query_node || !RewritePolicyQueryNode(*entry.second->query_node, ctx)) {
			ctx.Reject("policy enforcement could not inspect a common table expression");
			return false;
		}
	}
	return true;
}

bool InspectPolicySetInfo(UpdateSetInfo &set_info, PolicyRewriteContext &ctx) {
	if (set_info.condition && !RewritePolicyExpression(*set_info.condition, ctx)) {
		return false;
	}
	return InspectPolicyExpressionList(set_info.expressions, ctx);
}

//! Walk a statement the rewriter cannot express, reporting every relation it
//! reaches. Reuses the SELECT walkers so views, table macros and unsupported
//! sources stay refused rather than trusted, and treats a statement whose
//! relations cannot be enumerated at all as one of those.
bool InspectUnrewritableStatement(SQLStatement &statement, PolicyRewriteContext &ctx) {
	switch (statement.type) {
	case StatementType::SELECT_STATEMENT:
		return RewritePolicySelect(statement.Cast<SelectStatement>(), ctx);
	case StatementType::INSERT_STATEMENT: {
		auto &insert = *statement.Cast<InsertStatement>().node;
		auto target = make_uniq<BaseTableRef>();
		target->SetQualifiedName(insert.qualified_name);
		unique_ptr<TableRef> target_ref = std::move(target);
		if (!InspectPolicyCteMap(insert.cte_map, ctx) || !RewritePolicyTableRef(target_ref, ctx)) {
			return false;
		}
		if (insert.select_statement && !RewritePolicySelect(*insert.select_statement, ctx)) {
			return false;
		}
		if (insert.on_conflict_info) {
			auto &conflict = *insert.on_conflict_info;
			if (conflict.condition && !RewritePolicyExpression(*conflict.condition, ctx)) {
				return false;
			}
			if (conflict.set_info && !InspectPolicySetInfo(*conflict.set_info, ctx)) {
				return false;
			}
		}
		return InspectPolicyExpressionList(insert.returning_list, ctx);
	}
	case StatementType::UPDATE_STATEMENT: {
		auto &update = *statement.Cast<UpdateStatement>().node;
		if (!InspectPolicyCteMap(update.cte_map, ctx) || !RewritePolicyTableRef(update.table, ctx)) {
			return false;
		}
		if (update.from_table && !RewritePolicyTableRef(update.from_table, ctx)) {
			return false;
		}
		// UPDATE keeps its WHERE on set_info, not on the statement.
		if (update.set_info && !InspectPolicySetInfo(*update.set_info, ctx)) {
			return false;
		}
		return InspectPolicyExpressionList(update.returning_list, ctx);
	}
	case StatementType::DELETE_STATEMENT: {
		auto &remove = *statement.Cast<DeleteStatement>().node;
		if (!InspectPolicyCteMap(remove.cte_map, ctx) || !RewritePolicyTableRef(remove.table, ctx)) {
			return false;
		}
		for (auto &using_clause : remove.using_clauses) {
			if (!RewritePolicyTableRef(using_clause, ctx)) {
				return false;
			}
		}
		if (remove.condition && !RewritePolicyExpression(*remove.condition, ctx)) {
			return false;
		}
		return InspectPolicyExpressionList(remove.returning_list, ctx);
	}
	default:
		ctx.Reject(StringUtil::Format("policy enforcement cannot resolve the relations a %s handler reaches",
		                              StatementTypeToString(statement.type)));
		return false;
	}
}

//! Row-access and masking policies rewrite reads only, so a handler the rewriter
//! cannot express is admissible exactly when it reaches no bound relation.
//! Returns the refusal reason, empty when the handler may run unchanged.
string InspectUnrewritableHandler(DatabaseInstance &db, const vector<QuackapiRowAccessBinding> &rap_bindings,
                                  const vector<QuackapiMaskingBinding> &mask_bindings,
                                  vector<unique_ptr<SQLStatement>> &statements) {
	PolicyRewriteContext ctx(db, rap_bindings, mask_bindings, true);
	ctx.detect_only = true;
	for (auto &statement : statements) {
		if (statement && !InspectUnrewritableStatement(*statement, ctx)) {
			break;
		}
	}
	return ctx.error;
}

//! Parse and rewrite every supported SELECT shape. Callers receive an explicit
//! error when a protected relation cannot be inspected safely.
bool RewriteHandlerWithPoliciesAst(DatabaseInstance &db, const string &handler_sql, bool authenticated,
                                   bool &deny_unauthenticated, string &policy_error, string &rewritten_sql) {
	deny_unauthenticated = false;
	policy_error.clear();
	auto &state = QuackapiState::Get(db);
	auto rap_bindings = state.SnapshotRowAccessBindings();
	auto mask_bindings = state.SnapshotMaskingBindings();
	if (rap_bindings.empty() && mask_bindings.empty()) {
		rewritten_sql = handler_sql;
		return true;
	}
	PolicyRewriteContext ctx(db, rap_bindings, mask_bindings, authenticated);
	try {
		Parser parser;
		parser.ParseQuery(handler_sql);
		if (parser.statements.size() == 1 && parser.statements[0]->type == StatementType::SELECT_STATEMENT) {
			auto &select = parser.statements[0]->Cast<SelectStatement>();
			RewritePolicySelect(select, ctx);
			if (ctx.error.empty() && !ctx.deny_unauthenticated) {
				rewritten_sql = select.ToString();
			}
		} else {
			// Assigned rather than Reject()ed: an unsupported handler is not an
			// authentication failure, and folding it into deny_unauthenticated makes
			// every caller report the wrong reason.
			ctx.error = InspectUnrewritableHandler(db, rap_bindings, mask_bindings, parser.statements);
			if (ctx.error.empty()) {
				rewritten_sql = handler_sql;
			}
		}
	} catch (...) {
		ctx.Reject("policy enforcement could not parse handler SQL");
	}
	deny_unauthenticated = ctx.deny_unauthenticated;
	policy_error = ctx.error;
	if (policy_error.empty() && !deny_unauthenticated) {
		return true;
	}
	rewritten_sql = handler_sql;
	return true;
}

bool HandlerTouchesPoliciedTable(DatabaseInstance &db, const string &handler_sql) {
	bool deny_unauthenticated = false;
	string policy_error;
	string rewritten;
	RewriteHandlerWithPoliciesAst(db, handler_sql, false, deny_unauthenticated, policy_error, rewritten);
	return deny_unauthenticated || !policy_error.empty();
}

bool HandlerUnsupportedByPolicies(DatabaseInstance &db, const string &handler_sql, string &reason) {
	reason.clear();
	auto &state = QuackapiState::Get(db);
	auto rap_bindings = state.SnapshotRowAccessBindings();
	auto mask_bindings = state.SnapshotMaskingBindings();
	if (rap_bindings.empty() && mask_bindings.empty()) {
		return false;
	}
	try {
		Parser parser;
		parser.ParseQuery(handler_sql);
		if (parser.statements.size() == 1 && parser.statements[0]->type == StatementType::SELECT_STATEMENT) {
			return false;
		}
		reason = InspectUnrewritableHandler(db, rap_bindings, mask_bindings, parser.statements);
	} catch (...) {
		// Handler SQL that does not parse is already reported by the caller's own
		// prepare step, which quotes the parser's error instead of guessing.
		return false;
	}
	return !reason.empty();
}

string RewriteHandlerWithPolicies(DatabaseInstance &db, const string &handler_sql, bool authenticated,
                                  bool &deny_unauthenticated, string &policy_error) {
	string parsed_rewrite;
	RewriteHandlerWithPoliciesAst(db, handler_sql, authenticated, deny_unauthenticated, policy_error, parsed_rewrite);
	return parsed_rewrite;
}

PolicyDdlParserExtension::PolicyDdlParserExtension() {
	parse_function = PolicyDdlParse;
	plan_function = PolicyDdlPlan;
}

TableFunction GetApplyPolicyFunction() {
	return MakeApplyPolicyFunction();
}

TableFunction GetQuackapiPoliciesFunction() {
	return TableFunction("quackapi_policies", {}, PoliciesExec, PoliciesBind, PoliciesInit);
}

} // namespace duckdb
