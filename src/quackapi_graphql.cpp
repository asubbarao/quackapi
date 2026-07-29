#include "quackapi_graphql.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/query_result.hpp"

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

string ExecuteGraphqlQuery(DatabaseInstance &db, const string &query, idx_t limit) {
	auto doc = ParseThinGraphql(query);
	if (!doc.error.empty()) {
		return GraphqlError(doc.error);
	}
	if (limit == 0) {
		limit = QUACKAPI_GRAPHQL_DEFAULT_LIMIT;
	}

	Connection con(db);
	string data = "{";
	bool first = true;
	for (auto &field : doc.roots) {
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

string BuildGraphqlSchema(DatabaseInstance &db) {
	Connection con(db);
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

	string body = "{\"tables\":{";
	bool first_table = true;
	while (true) {
		auto chunk = res->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		for (idx_t r = 0; r < chunk->size(); r++) {
			if (!first_table) {
				body += ",";
			}
			first_table = false;
			auto table = chunk->GetValue(0, r).ToString();
			body += "\"" + QuackapiJsonEscape(table) + "\":[";
			// cols is a LIST
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
	body += "},\"note\":\"GraphQL v0 catalog-only schema. Full __schema introspection not implemented. "
	        "Future: sitting_duck/parser_tools may feed types from external code.\"}";
	return body;
}

} // namespace duckdb
