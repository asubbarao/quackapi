#include "quackapi_policy.hpp"

#include <algorithm>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parser_extension.hpp"

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
	string s = Trim(inner);
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

string QuoteIdentIfNeeded(const string &ident) {
	bool needs = false;
	if (ident.empty() || !IsIdentStart(ident[0])) {
		needs = true;
	} else {
		for (char c : ident) {
			if (!IsIdentChar(c)) {
				needs = true;
				break;
			}
		}
	}
	if (!needs) {
		// reserved-ish tokens still fine as bare for table names we control
		return ident;
	}
	string escaped;
	for (char c : ident) {
		if (c == '"') {
			escaped += "\"\"";
		} else {
			escaped += c;
		}
	}
	return "\"" + escaped + "\"";
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

//! SQL keywords that cannot be a table alias when following a table name.
bool IsSqlClauseKeyword(const string &tok_upper) {
	return tok_upper == "WHERE" || tok_upper == "JOIN" || tok_upper == "LEFT" || tok_upper == "RIGHT" ||
	       tok_upper == "INNER" || tok_upper == "OUTER" || tok_upper == "FULL" || tok_upper == "CROSS" ||
	       tok_upper == "ON" || tok_upper == "GROUP" || tok_upper == "ORDER" || tok_upper == "LIMIT" ||
	       tok_upper == "OFFSET" || tok_upper == "HAVING" || tok_upper == "UNION" || tok_upper == "EXCEPT" ||
	       tok_upper == "INTERSECT" || tok_upper == "RETURNING" || tok_upper == "WINDOW" || tok_upper == "QUALIFY" ||
	       tok_upper == "USING" || tok_upper == "NATURAL" || tok_upper == "AS" || tok_upper == "FROM" ||
	       tok_upper == "SELECT" || tok_upper == "WITH" || tok_upper == "AND" || tok_upper == "OR";
}

//! Build secure subquery body (no outer alias) for one table.
string BuildSecureSubquery(const string &table, const QuackapiRowAccessPolicy *rap,
                           const vector<std::pair<string, string>> &masked_cols /* col -> expr with val subbed */) {
	string t = QuoteIdentIfNeeded(table);
	string where_sql;
	if (rap) {
		// Rewrite policy arg names → bound table column names when they differ.
		// Convention: expression uses signature names; binding ON (cols) maps by position.
		// When names match (common case) expression is used as-is.
		where_sql = " WHERE (" + rap->expression + ")";
	}
	if (masked_cols.empty()) {
		return "SELECT * FROM " + t + where_sql;
	}
	string excludes;
	string extras;
	for (idx_t i = 0; i < masked_cols.size(); i++) {
		if (i > 0) {
			excludes += ", ";
			extras += ", ";
		}
		auto colq = QuoteIdentIfNeeded(masked_cols[i].first);
		excludes += colq;
		extras += "(" + masked_cols[i].second + ") AS " + colq;
	}
	return "SELECT * EXCLUDE (" + excludes + "), " + extras + " FROM " + t + where_sql;
}

//! Replace identifier occurrences of `table` with `(secure) AS alias` in SQL.
string RewriteTableRefs(const string &sql, const string &table, const string &secure_body) {
	string result;
	result.reserve(sql.size() + secure_body.size());
	bool in_str = false;
	idx_t i = 0;
	string table_lower = StringUtil::Lower(table);
	while (i < sql.size()) {
		char c = sql[i];
		if (in_str) {
			result += c;
			if (c == '\'') {
				if (i + 1 < sql.size() && sql[i + 1] == '\'') {
					result += sql[i + 1];
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
			result += c;
			i++;
			continue;
		}
		// Skip already-rewritten `(SELECT … FROM table …)` inner occurrences: only
		// replace when the preceding non-space char is not '(' after FROM was already
		// expanded. Simpler: match whole identifier equal to table.
		if (IsIdentStart(c) || (c >= '0' && c <= '9')) {
			// Only start identifiers at alpha/_
			if (!IsIdentStart(c)) {
				result += c;
				i++;
				continue;
			}
			idx_t j = i + 1;
			while (j < sql.size() && IsIdentChar(sql[j])) {
				j++;
			}
			string tok = sql.substr(i, j - i);
			if (StringUtil::Lower(tok) != table_lower) {
				result += tok;
				i = j;
				continue;
			}
			// Word-boundary: previous char must not be ident (already true) and next
			// already non-ident. Check not schema-qualified prefix (skip "schema.table"
			// when we only registered bare table) — if prev is '.', still rewrite
			// (rare); operators leave it.
			// Consume optional alias: [AS] alias
			idx_t k = j;
			while (k < sql.size() && StringUtil::CharacterIsSpace(sql[k])) {
				k++;
			}
			string alias = table;
			if (k < sql.size()) {
				// AS alias
				if (IsIdentStart(sql[k])) {
					idx_t a0 = k;
					idx_t a1 = k + 1;
					while (a1 < sql.size() && IsIdentChar(sql[a1])) {
						a1++;
					}
					string maybe = sql.substr(a0, a1 - a0);
					auto mu = StringUtil::Upper(maybe);
					if (mu == "AS") {
						k = a1;
						while (k < sql.size() && StringUtil::CharacterIsSpace(sql[k])) {
							k++;
						}
						if (k < sql.size() && IsIdentStart(sql[k])) {
							idx_t b0 = k;
							idx_t b1 = k + 1;
							while (b1 < sql.size() && IsIdentChar(sql[b1])) {
								b1++;
							}
							alias = sql.substr(b0, b1 - b0);
							j = b1;
						}
					} else if (!IsSqlClauseKeyword(mu)) {
						alias = maybe;
						j = a1;
					}
				}
			}
			// Avoid double-wrapping: if already `(SELECT …) AS table` from a prior pass
			// for the same table, the outer alias is still the table name — but the
			// inner FROM table is still present. We only rewrite outer refs by doing
			// a single pass left-to-right; inner FROM table is rewritten too which
			// would recurse. Solution: rewrite ONLY when not immediately after FROM
			// inside a secure wrapper is hard.
			// Instead: build secure with the physical table quoted, and mark with a
			// sentinel so we don't re-match. Use quoted "table" form inside secure
			// subquery which won't match bare identifier scan of bare table name
			// when table is unquoted identifier... Actually QuoteIdentIfNeeded leaves
			// bare names bare. Force-quote the physical table inside the subquery.
			result += "(" + secure_body + ") AS " + QuoteIdentIfNeeded(alias);
			i = j;
			continue;
		}
		result += c;
		i++;
	}
	return result;
}

// Force double-quote physical table so RewriteTableRefs won't rematch inside body.
string BuildSecureSubqueryQuoted(const string &table, const QuackapiRowAccessPolicy *rap_for_where,
                                 const string &where_expr_or_empty,
                                 const vector<std::pair<string, string>> &masked_cols) {
	// Always quote physical base table.
	string t = "\"" + table + "\"";
	// Escape embedded quotes in table name
	{
		string esc;
		for (char c : table) {
			if (c == '"') {
				esc += "\"\"";
			} else {
				esc += c;
			}
		}
		t = "\"" + esc + "\"";
	}
	string where_sql;
	if (!where_expr_or_empty.empty()) {
		where_sql = " WHERE (" + where_expr_or_empty + ")";
	}
	(void)rap_for_where;
	if (masked_cols.empty()) {
		return "SELECT * FROM " + t + where_sql;
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
	return "SELECT * EXCLUDE (" + excludes + "), " + extras + " FROM " + t + where_sql;
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

ParserExtensionParseResult PolicyDdlParse(ParserExtensionInfo *, const string &query) {
	auto q = Trim(query);
	auto upper = StringUtil::Upper(q);

	// ---- DROP ROW ACCESS POLICY / DROP MASKING POLICY ----
	if (StringUtil::StartsWith(upper, "DROP ROW ACCESS POLICY ")) {
		auto name = Trim(q.substr(23));
		if (name.empty() || name.find(' ') != string::npos) {
			return ParserExtensionParseResult("DROP ROW ACCESS POLICY expects a single policy name");
		}
		auto data = make_uniq<PolicyDdlParseData>();
		data->action = "DROP_ROW";
		data->name = name;
		return ParserExtensionParseResult(std::move(data));
	}
	if (StringUtil::StartsWith(upper, "DROP MASKING POLICY ")) {
		auto name = Trim(q.substr(20));
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
		auto rest = Trim(q.substr(pos));
		idx_t name_len = 0;
		auto name = NextToken(rest, name_len);
		if (name.empty() || name_len >= rest.size()) {
			return ParserExtensionParseResult(
			    "CREATE ROW ACCESS POLICY <name> AS (<cols>) RETURNS BOOLEAN USING (<expr>)");
		}
		rest = Trim(rest.substr(name_len));
		auto ru = StringUtil::Upper(rest);
		if (!StringUtil::StartsWith(ru, "AS")) {
			return ParserExtensionParseResult("Expected AS (<cols>) after policy name");
		}
		rest = Trim(rest.substr(2));
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
		rest = Trim(rest.substr(after_cols));
		ru = StringUtil::Upper(rest);
		// RETURNS BOOLEAN
		if (!StringUtil::StartsWith(ru, "RETURNS")) {
			return ParserExtensionParseResult("Expected RETURNS BOOLEAN after AS (...)");
		}
		rest = Trim(rest.substr(7));
		ru = StringUtil::Upper(rest);
		if (!StringUtil::StartsWith(ru, "BOOLEAN")) {
			return ParserExtensionParseResult("ROW ACCESS POLICY must RETURNS BOOLEAN");
		}
		rest = Trim(rest.substr(7));
		ru = StringUtil::Upper(rest);
		if (!StringUtil::StartsWith(ru, "USING")) {
			return ParserExtensionParseResult("Expected USING (<expr>) after RETURNS BOOLEAN");
		}
		rest = Trim(rest.substr(5));
		if (rest.empty() || rest[0] != '(') {
			return ParserExtensionParseResult("Expected USING (<expr>)");
		}
		string expr;
		idx_t after_expr = 0;
		if (!ExtractParenGroup(rest, 0, expr, after_expr)) {
			return ParserExtensionParseResult("Unterminated USING (...) expression");
		}
		expr = Trim(expr);
		if (expr.empty()) {
			return ParserExtensionParseResult("USING expression must not be empty");
		}
		rest = Trim(rest.substr(after_expr));
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
		auto rest = Trim(q.substr(pos));
		idx_t name_len = 0;
		auto name = NextToken(rest, name_len);
		if (name.empty() || name_len >= rest.size()) {
			return ParserExtensionParseResult("CREATE MASKING POLICY <name> ON <type> USING (<expr>)");
		}
		rest = Trim(rest.substr(name_len));
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
		rest = Trim(rest.substr(2));
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
		rest = Trim(rest.substr(type_len));
		ru = StringUtil::Upper(rest);
		if (!StringUtil::StartsWith(ru, "USING")) {
			return ParserExtensionParseResult("Expected USING (<expr>) after ON <type>");
		}
		rest = Trim(rest.substr(5));
		if (rest.empty() || rest[0] != '(') {
			return ParserExtensionParseResult("Expected USING (<expr>)");
		}
		string expr;
		idx_t after_expr = 0;
		if (!ExtractParenGroup(rest, 0, expr, after_expr)) {
			return ParserExtensionParseResult("Unterminated USING (...) expression");
		}
		expr = Trim(expr);
		if (expr.empty()) {
			return ParserExtensionParseResult("USING expression must not be empty");
		}
		rest = Trim(rest.substr(after_expr));
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
		auto rest = Trim(q.substr(12));
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
					rest = Trim(rest.substr(i + 1));
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
			rest = Trim(rest.substr(i));
		}
		auto ru = StringUtil::Upper(rest);

		// ADD [OR REPLACE] ROW ACCESS POLICY <p> ON (<cols>)
		bool add_or_replace = false;
		if (StringUtil::StartsWith(ru, "ADD OR REPLACE ROW ACCESS POLICY ")) {
			rest = Trim(rest.substr(33));
			add_or_replace = true;
		} else if (StringUtil::StartsWith(ru, "ADD ROW ACCESS POLICY ")) {
			rest = Trim(rest.substr(22));
		} else if (StringUtil::StartsWith(ru, "DROP ROW ACCESS POLICY ")) {
			auto pname = Trim(rest.substr(23));
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
			rest = Trim(rest.substr(skip));
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
			rest = Trim(rest.substr(ci));
			ru = StringUtil::Upper(rest);
			if (StringUtil::StartsWith(ru, "SET MASKING POLICY ")) {
				auto pname = Trim(rest.substr(19));
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
		rest = pname_len >= rest.size() ? string() : Trim(rest.substr(pname_len));
		ru = StringUtil::Upper(rest);
		if (!StringUtil::StartsWith(ru, "ON")) {
			return ParserExtensionParseResult("Expected ON (<cols>) after policy name");
		}
		rest = Trim(rest.substr(2));
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
		rest = Trim(rest.substr(after));
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
		out.push_back(Trim(csv.substr(i, j - i)));
		i = j < csv.size() ? j + 1 : j;
	}
	return out;
}

unique_ptr<FunctionData> ApplyPolicyBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
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
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("status");
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

	output.SetValue(0, 0, Value(message));
	output.SetCardinality(1);
	bind_data.finished = true;
}

TableFunction MakeApplyPolicyFunction() {
	// action, or_replace, name, value_type, arg_columns_csv, arg_types_csv, expression, table, column
	TableFunction function("quackapi_apply_policy",
	                       {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                        LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                        LogicalType::VARCHAR},
	                       ApplyPolicyExec, ApplyPolicyBind);
	return function;
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
	result.requires_valid_transaction = false;
	result.return_type = StatementReturnType::QUERY_RESULT;
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
                                      vector<string> &names) {
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

bool HandlerTouchesPoliciedTable(DatabaseInstance &db, const string &handler_sql) {
	auto &state = QuackapiState::Get(db);
	auto rap_binds = state.SnapshotRowAccessBindings();
	auto mask_binds = state.SnapshotMaskingBindings();
	string sql_lower = StringUtil::Lower(handler_sql);
	auto touches = [&](const string &table) -> bool {
		string t = StringUtil::Lower(table);
		// crude but sufficient: whole-word match
		idx_t pos = 0;
		while (pos < sql_lower.size()) {
			auto found = sql_lower.find(t, pos);
			if (found == string::npos) {
				return false;
			}
			bool left_ok = found == 0 || !IsIdentChar(sql_lower[found - 1]);
			idx_t end = found + t.size();
			bool right_ok = end >= sql_lower.size() || !IsIdentChar(sql_lower[end]);
			if (left_ok && right_ok) {
				return true;
			}
			pos = found + 1;
		}
		return false;
	};
	for (auto &b : rap_binds) {
		if (touches(b.table_name)) {
			return true;
		}
	}
	for (auto &b : mask_binds) {
		if (touches(b.table_name)) {
			return true;
		}
	}
	return false;
}

string RewriteHandlerWithPolicies(DatabaseInstance &db, const string &handler_sql, bool authenticated,
                                  bool &deny_unauthenticated) {
	deny_unauthenticated = false;
	auto &state = QuackapiState::Get(db);
	auto rap_binds = state.SnapshotRowAccessBindings();
	auto mask_binds = state.SnapshotMaskingBindings();
	if (rap_binds.empty() && mask_binds.empty()) {
		return handler_sql;
	}

	// Collect tables that appear in the handler and have bindings.
	unordered_map<string, string> table_canonical; // lower -> original
	auto consider = [&](const string &table) {
		if (HandlerTouchesPoliciedTable(db, handler_sql)) {
			// per-table check
		}
		string sql_lower = StringUtil::Lower(handler_sql);
		string t = StringUtil::Lower(table);
		idx_t pos = 0;
		while (pos < sql_lower.size()) {
			auto found = sql_lower.find(t, pos);
			if (found == string::npos) {
				break;
			}
			bool left_ok = found == 0 || !IsIdentChar(sql_lower[found - 1]);
			idx_t end = found + t.size();
			bool right_ok = end >= sql_lower.size() || !IsIdentChar(sql_lower[end]);
			if (left_ok && right_ok) {
				table_canonical[t] = table;
				return;
			}
			pos = found + 1;
		}
	};
	for (auto &b : rap_binds) {
		consider(b.table_name);
	}
	for (auto &b : mask_binds) {
		consider(b.table_name);
	}
	if (table_canonical.empty()) {
		return handler_sql;
	}

	if (!authenticated) {
		deny_unauthenticated = true;
		return handler_sql;
	}

	// Longest table name first so "order_items" before "orders" if both exist.
	vector<string> tables;
	for (auto &kv : table_canonical) {
		tables.push_back(kv.second);
	}
	std::sort(tables.begin(), tables.end(), [](const string &a, const string &b) { return a.size() > b.size(); });

	string sql = handler_sql;
	for (auto &table : tables) {
		// Resolve RAP
		const QuackapiRowAccessPolicy *rap = nullptr;
		QuackapiRowAccessPolicy rap_storage;
		string where_expr;
		for (auto &b : rap_binds) {
			if (StringUtil::Lower(b.table_name) != StringUtil::Lower(table)) {
				continue;
			}
			if (!state.GetRowAccessPolicy(b.policy_name, rap_storage)) {
				continue;
			}
			// Map signature column names in expression → bound table columns.
			where_expr = rap_storage.expression;
			for (idx_t i = 0; i < rap_storage.arg_columns.size() && i < b.columns.size(); i++) {
				if (StringUtil::Lower(rap_storage.arg_columns[i]) == StringUtil::Lower(b.columns[i])) {
					continue;
				}
				// Replace whole-word arg name with bound column.
				string mapped;
				bool in_str = false;
				string src = where_expr;
				string from = rap_storage.arg_columns[i];
				string to = b.columns[i];
				for (idx_t j = 0; j < src.size();) {
					if (in_str) {
						mapped += src[j];
						if (src[j] == '\'') {
							if (j + 1 < src.size() && src[j + 1] == '\'') {
								mapped += src[j + 1];
								j += 2;
								continue;
							}
							in_str = false;
						}
						j++;
						continue;
					}
					if (src[j] == '\'') {
						in_str = true;
						mapped += src[j++];
						continue;
					}
					if (IsIdentStart(src[j])) {
						idx_t k = j + 1;
						while (k < src.size() && IsIdentChar(src[k])) {
							k++;
						}
						string tok = src.substr(j, k - j);
						if (StringUtil::Lower(tok) == StringUtil::Lower(from)) {
							mapped += to;
						} else {
							mapped += tok;
						}
						j = k;
						continue;
					}
					mapped += src[j++];
				}
				where_expr = mapped;
			}
			rap = &rap_storage;
			break;
		}

		// Masks for this table
		vector<std::pair<string, string>> masked;
		for (auto &b : mask_binds) {
			if (StringUtil::Lower(b.table_name) != StringUtil::Lower(table)) {
				continue;
			}
			QuackapiMaskingPolicy mp;
			if (!state.GetMaskingPolicy(b.policy_name, mp)) {
				continue;
			}
			// Physical column in subquery must be quoted so rewrite doesn't rematch.
			string col_esc;
			for (char c : b.column_name) {
				if (c == '"') {
					col_esc += "\"\"";
				} else {
					col_esc += c;
				}
			}
			string quoted_col = "\"" + col_esc + "\"";
			string expr = SubstituteValPlaceholder(mp.expression, quoted_col);
			masked.emplace_back(b.column_name, expr);
		}

		string body = BuildSecureSubqueryQuoted(table, rap, where_expr, masked);
		sql = RewriteTableRefs(sql, table, body);
	}
	return sql;
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
