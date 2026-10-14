#include "quackapi_validation.hpp"

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/connection.hpp"

#include "quackapi_util.hpp"

namespace duckdb {

namespace {

enum class JsonNodeKind : uint8_t { NIL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

struct JsonNode {
	JsonNodeKind kind = JsonNodeKind::NIL;
	idx_t id = 0;
	string key;
	string scalar;
};

struct JsonTree {
	vector<JsonNode> nodes;
	unordered_map<idx_t, vector<idx_t>> children_by_parent;
};

bool IsMissingJsonExtension(const string &error) {
	return error.find("json extension") != string::npos;
}

JsonNodeKind JsonTypeToKind(const string &type) {
	if (type == "OBJECT") {
		return JsonNodeKind::OBJECT;
	}
	if (type == "ARRAY") {
		return JsonNodeKind::ARRAY;
	}
	if (type == "VARCHAR") {
		return JsonNodeKind::STRING;
	}
	if (type == "BOOLEAN") {
		return JsonNodeKind::BOOL;
	}
	if (type == "TINYINT" || type == "SMALLINT" || type == "INTEGER" || type == "BIGINT" || type == "HUGEINT" ||
	    type == "UTINYINT" || type == "USMALLINT" || type == "UINTEGER" || type == "UBIGINT" || type == "UHUGEINT" ||
	    type == "DECIMAL" || type == "FLOAT" || type == "DOUBLE") {
		return JsonNodeKind::NUMBER;
	}
	return JsonNodeKind::NIL;
}

//! Materialize one DuckDB-native JSON traversal. json_tree owns parsing,
//! escaping, Unicode handling, and node classification; this code only stores
//! its rows so callers can walk the resulting tree without reparsing input.
bool ReadJsonTree(Connection &con, const string &json, JsonTree &tree) {
	const string sql = "SELECT id, parent, key, type, CASE WHEN type = 'VARCHAR' THEN json_extract_string(atom, '$') "
	                   "ELSE atom::VARCHAR END "
	                   "FROM json_tree(?::JSON) ORDER BY id";
	auto result = con.Query(sql, Value(json));
	if (result->HasError() && IsMissingJsonExtension(result->GetError())) {
		auto load = con.Query("LOAD json");
		if (!load->HasError()) {
			result = con.Query(sql, Value(json));
		}
	}
	if (result->HasError()) {
		return false;
	}
	auto chunk = result->Fetch();
	while (chunk && chunk->size() > 0) {
		for (idx_t row = 0; row < chunk->size(); row++) {
			JsonNode node;
			node.id = chunk->GetValue(0, row).GetValue<uint64_t>();
			auto parent = chunk->GetValue(1, row);
			auto key = chunk->GetValue(2, row);
			if (!key.IsNull()) {
				node.key = key.GetValue<string>();
			}
			auto type = chunk->GetValue(3, row).GetValue<string>();
			node.kind = JsonTypeToKind(type);
			auto scalar = chunk->GetValue(4, row);
			if (!scalar.IsNull()) {
				node.scalar = scalar.GetValue<string>();
			}
			const auto index = tree.nodes.size();
			if (!parent.IsNull()) {
				tree.children_by_parent[parent.GetValue<uint64_t>()].push_back(index);
			}
			tree.nodes.push_back(std::move(node));
		}
		chunk = result->Fetch();
	}
	return !tree.nodes.empty();
}

bool IsCanonicalArrayIndex(const string &segment) {
	if (segment.empty() || (segment.size() > 1 && segment[0] == '0')) {
		return false;
	}
	for (auto c : segment) {
		if (c < '0' || c > '9') {
			return false;
		}
	}
	return true;
}

string DecodeJsonPointerSegment(const string &segment) {
	string result;
	result.reserve(segment.size());
	for (idx_t i = 0; i < segment.size(); i++) {
		if (segment[i] == '~' && i + 1 < segment.size()) {
			if (segment[i + 1] == '0') {
				result += '~';
				i++;
				continue;
			}
			if (segment[i + 1] == '1') {
				result += '/';
				i++;
				continue;
			}
		}
		result += segment[i];
	}
	return result;
}

const JsonNode *FindChild(const JsonTree &tree, const JsonNode &parent, const string &key) {
	auto children = tree.children_by_parent.find(parent.id);
	if (children == tree.children_by_parent.end()) {
		return nullptr;
	}
	for (auto child_index : children->second) {
		const auto &child = tree.nodes[child_index];
		if (child.key == key) {
			return &child;
		}
	}
	return nullptr;
}

string TransformScalarToOpenApi(const string &declared_type) {
	string type = declared_type;
	StringUtil::Trim(type);
	type = StringUtil::Upper(type);
	if (type == "BOOLEAN" || type == "BOOL") {
		return "{\"type\":\"boolean\"}";
	}
	if (type == "TINYINT" || type == "SMALLINT" || type == "INTEGER" || type == "INT" || type == "UTINYINT" ||
	    type == "USMALLINT") {
		return "{\"type\":\"integer\",\"format\":\"int32\"}";
	}
	if (type == "BIGINT" || type == "UINTEGER") {
		return "{\"type\":\"integer\",\"format\":\"int64\"}";
	}
	if (type == "HUGEINT" || type == "UBIGINT" || type == "UHUGEINT") {
		return "{\"type\":\"integer\"}";
	}
	if (type == "FLOAT" || type == "REAL") {
		return "{\"type\":\"number\",\"format\":\"float\"}";
	}
	if (type == "DOUBLE" || StringUtil::StartsWith(type, "DECIMAL") || StringUtil::StartsWith(type, "NUMERIC")) {
		return "{\"type\":\"number\"}";
	}
	if (type == "DATE") {
		return "{\"type\":\"string\",\"format\":\"date\"}";
	}
	if (StringUtil::StartsWith(type, "TIMESTAMP")) {
		return "{\"type\":\"string\",\"format\":\"date-time\"}";
	}
	if (type == "UUID") {
		return "{\"type\":\"string\",\"format\":\"uuid\"}";
	}
	if (type == "JSON" || type == "ANY") {
		return "{}";
	}
	return "{\"type\":\"string\"}";
}

string TransformNodeToOpenApi(const JsonTree &tree, idx_t node_index) {
	const auto &node = tree.nodes[node_index];
	switch (node.kind) {
	case JsonNodeKind::STRING:
		return TransformScalarToOpenApi(node.scalar);
	case JsonNodeKind::ARRAY: {
		auto children = tree.children_by_parent.find(node.id);
		return "{\"type\":\"array\",\"items\":" +
		       (children == tree.children_by_parent.end() || children->second.empty()
		            ? string("{}")
		            : TransformNodeToOpenApi(tree, children->second[0])) +
		       "}";
	}
	case JsonNodeKind::OBJECT: {
		string result = "{\"type\":\"object\",\"properties\":{";
		auto children = tree.children_by_parent.find(node.id);
		if (children != tree.children_by_parent.end()) {
			bool first = true;
			for (auto child_index : children->second) {
				const auto &child = tree.nodes[child_index];
				if (!first) {
					result += ",";
				}
				first = false;
				result += "\"" + QuackapiJsonEscape(child.key) + "\":" + TransformNodeToOpenApi(tree, child_index);
			}
		}
		return result + "}}";
	}
	default:
		return "{}";
	}
}

} // namespace

string QuackapiValidationBodyPointerLoc(Connection &con, const string &pointer, const string &json_body) {
	string result = "[\"body\"";
	if (pointer.empty()) {
		return result + "]";
	}
	JsonTree tree;
	const JsonNode *current = ReadJsonTree(con, json_body, tree) ? &tree.nodes[0] : nullptr;
	idx_t start = pointer.size() > 0 && pointer[0] == '/' ? 1 : 0;
	while (true) {
		auto end = pointer.find('/', start);
		auto raw_segment = pointer.substr(start, end == string::npos ? string::npos : end - start);
		auto segment = DecodeJsonPointerSegment(raw_segment);
		result += ",";
		if (current && current->kind == JsonNodeKind::ARRAY && IsCanonicalArrayIndex(segment)) {
			result += segment;
			current = FindChild(tree, *current, segment);
		} else {
			result += "\"" + QuackapiJsonEscape(segment) + "\"";
			current = current && current->kind == JsonNodeKind::OBJECT ? FindChild(tree, *current, segment) : nullptr;
		}
		if (end == string::npos) {
			break;
		}
		start = end + 1;
	}
	return result + "]";
}

string QuackapiDuckdbTransformToOpenApi(Connection &con, const string &body_type) {
	JsonTree tree;
	if (!ReadJsonTree(con, body_type, tree)) {
		return "{}";
	}
	return TransformNodeToOpenApi(tree, 0);
}

string QuackapiValidationErrorsJson(const vector<QuackapiValidationIssue> &issues) {
	string result = "{\"detail\":[";
	for (idx_t i = 0; i < issues.size(); i++) {
		if (i > 0) {
			result += ",";
		}
		auto &issue = issues[i];
		result += "{\"loc\":" + issue.loc_json + ",\"msg\":\"" + QuackapiJsonEscape(issue.message) + "\",\"type\":\"" +
		          QuackapiJsonEscape(issue.type) + "\"}";
	}
	return result + "]}";
}

} // namespace duckdb
