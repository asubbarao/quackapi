#include "quackapi_validation.hpp"

#include "quackapi_util.hpp"

namespace duckdb {

namespace {

enum class JsonNodeKind : uint8_t { NIL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

struct JsonNode {
	JsonNodeKind kind = JsonNodeKind::NIL;
	string scalar;
	vector<JsonNode> children;
	vector<pair<string, JsonNode>> members;
};

//! Small, dependency-free JSON reader used only for declared structures and
//! error locations. DuckDB still owns request parsing and type validation.
class JsonReader {
public:
	explicit JsonReader(const string &input_p) : input(input_p) {
	}

	bool ParseDocument(JsonNode &result) {
		SkipWhitespace();
		if (!ParseValue(result, 0)) {
			return false;
		}
		SkipWhitespace();
		return position == input.size();
	}

private:
	static constexpr idx_t MAX_NESTING_DEPTH = 128;
	const string &input;
	idx_t position = 0;

	void SkipWhitespace() {
		while (position < input.size() && (input[position] == ' ' || input[position] == '\n' ||
		                                   input[position] == '\r' || input[position] == '\t')) {
			position++;
		}
	}

	bool Consume(char expected) {
		SkipWhitespace();
		if (position >= input.size() || input[position] != expected) {
			return false;
		}
		position++;
		return true;
	}

	static int HexValue(char c) {
		if (c >= '0' && c <= '9') {
			return c - '0';
		}
		if (c >= 'a' && c <= 'f') {
			return 10 + c - 'a';
		}
		if (c >= 'A' && c <= 'F') {
			return 10 + c - 'A';
		}
		return -1;
	}

	static void AppendUtf8(string &target, uint32_t codepoint) {
		if (codepoint <= 0x7f) {
			target += char(codepoint);
		} else if (codepoint <= 0x7ff) {
			target += char(0xc0 | (codepoint >> 6));
			target += char(0x80 | (codepoint & 0x3f));
		} else if (codepoint <= 0xffff) {
			target += char(0xe0 | (codepoint >> 12));
			target += char(0x80 | ((codepoint >> 6) & 0x3f));
			target += char(0x80 | (codepoint & 0x3f));
		} else {
			target += char(0xf0 | (codepoint >> 18));
			target += char(0x80 | ((codepoint >> 12) & 0x3f));
			target += char(0x80 | ((codepoint >> 6) & 0x3f));
			target += char(0x80 | (codepoint & 0x3f));
		}
	}

	bool ParseUnicodeEscape(uint32_t &codepoint) {
		if (position + 4 > input.size()) {
			return false;
		}
		codepoint = 0;
		for (idx_t i = 0; i < 4; i++) {
			auto hex = HexValue(input[position++]);
			if (hex < 0) {
				return false;
			}
			codepoint = (codepoint << 4) | uint32_t(hex);
		}
		return true;
	}

	bool ParseString(string &result) {
		if (!Consume('"')) {
			return false;
		}
		result.clear();
		while (position < input.size()) {
			auto c = input[position++];
			if (c == '"') {
				return true;
			}
			if (uint8_t(c) < 0x20) {
				return false;
			}
			if (c != '\\') {
				result += c;
				continue;
			}
			if (position >= input.size()) {
				return false;
			}
			auto escaped = input[position++];
			switch (escaped) {
			case '"':
			case '\\':
			case '/':
				result += escaped;
				break;
			case 'b':
				result += '\b';
				break;
			case 'f':
				result += '\f';
				break;
			case 'n':
				result += '\n';
				break;
			case 'r':
				result += '\r';
				break;
			case 't':
				result += '\t';
				break;
			case 'u': {
				uint32_t codepoint;
				if (!ParseUnicodeEscape(codepoint)) {
					return false;
				}
				if (codepoint >= 0xd800 && codepoint <= 0xdbff && position + 6 <= input.size() &&
				    input[position] == '\\' && input[position + 1] == 'u') {
					position += 2;
					uint32_t low;
					if (!ParseUnicodeEscape(low) || low < 0xdc00 || low > 0xdfff) {
						return false;
					}
					codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
				}
				AppendUtf8(result, codepoint);
				break;
			}
			default:
				return false;
			}
		}
		return false;
	}

	bool ParseValue(JsonNode &result, idx_t depth) {
		SkipWhitespace();
		if (position >= input.size()) {
			return false;
		}
		auto c = input[position];
		if (c == '"') {
			result.kind = JsonNodeKind::STRING;
			return ParseString(result.scalar);
		}
		if (c == '{') {
			return depth < MAX_NESTING_DEPTH && ParseObject(result, depth);
		}
		if (c == '[') {
			return depth < MAX_NESTING_DEPTH && ParseArray(result, depth);
		}
		if (input.compare(position, 4, "true") == 0 || input.compare(position, 5, "false") == 0) {
			result.kind = JsonNodeKind::BOOL;
			position += input.compare(position, 4, "true") == 0 ? 4 : 5;
			return true;
		}
		if (input.compare(position, 4, "null") == 0) {
			result.kind = JsonNodeKind::NIL;
			position += 4;
			return true;
		}
		if (c == '-' || (c >= '0' && c <= '9')) {
			auto start = position++;
			while (position < input.size()) {
				auto number_char = input[position];
				if (!((number_char >= '0' && number_char <= '9') || number_char == '.' || number_char == 'e' ||
				      number_char == 'E' || number_char == '+' || number_char == '-')) {
					break;
				}
				position++;
			}
			result.kind = JsonNodeKind::NUMBER;
			result.scalar = input.substr(start, position - start);
			return true;
		}
		return false;
	}

	bool ParseArray(JsonNode &result, idx_t depth) {
		if (!Consume('[')) {
			return false;
		}
		result.kind = JsonNodeKind::ARRAY;
		result.children.clear();
		SkipWhitespace();
		if (position < input.size() && input[position] == ']') {
			position++;
			return true;
		}
		while (true) {
			JsonNode child;
			if (!ParseValue(child, depth + 1)) {
				return false;
			}
			result.children.push_back(std::move(child));
			SkipWhitespace();
			if (position >= input.size()) {
				return false;
			}
			if (input[position] == ']') {
				position++;
				return true;
			}
			if (input[position++] != ',') {
				return false;
			}
		}
	}

	bool ParseObject(JsonNode &result, idx_t depth) {
		if (!Consume('{')) {
			return false;
		}
		result.kind = JsonNodeKind::OBJECT;
		result.members.clear();
		SkipWhitespace();
		if (position < input.size() && input[position] == '}') {
			position++;
			return true;
		}
		while (true) {
			string key;
			if (!ParseString(key) || !Consume(':')) {
				return false;
			}
			JsonNode value;
			if (!ParseValue(value, depth + 1)) {
				return false;
			}
			result.members.emplace_back(std::move(key), std::move(value));
			SkipWhitespace();
			if (position >= input.size()) {
				return false;
			}
			if (input[position] == '}') {
				position++;
				return true;
			}
			if (input[position++] != ',') {
				return false;
			}
		}
	}
};

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

const JsonNode *FindMember(const JsonNode &node, const string &name) {
	for (auto &member : node.members) {
		if (member.first == name) {
			return &member.second;
		}
	}
	return nullptr;
}

idx_t ParseIndex(const string &segment) {
	idx_t index = 0;
	for (auto c : segment) {
		auto digit = idx_t(c - '0');
		if (index > (idx_t(-1) - digit) / 10) {
			return idx_t(-1);
		}
		index = index * 10 + digit;
	}
	return index;
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

string TransformNodeToOpenApi(const JsonNode &node) {
	switch (node.kind) {
	case JsonNodeKind::STRING:
		return TransformScalarToOpenApi(node.scalar);
	case JsonNodeKind::ARRAY:
		return "{\"type\":\"array\",\"items\":" +
		       (node.children.empty() ? string("{}") : TransformNodeToOpenApi(node.children[0])) + "}";
	case JsonNodeKind::OBJECT: {
		string result = "{\"type\":\"object\",\"properties\":{";
		for (idx_t i = 0; i < node.members.size(); i++) {
			if (i > 0) {
				result += ",";
			}
			result += "\"" + QuackapiJsonEscape(node.members[i].first) +
			          "\":" + TransformNodeToOpenApi(node.members[i].second);
		}
		return result + "}}";
	}
	default:
		return "{}";
	}
}

} // namespace

string QuackapiValidationBodyPointerLoc(const string &pointer, const string &json_body) {
	string result = "[\"body\"";
	if (pointer.empty()) {
		return result + "]";
	}
	JsonNode root;
	const JsonNode *current = JsonReader(json_body).ParseDocument(root) ? &root : nullptr;
	idx_t start = pointer.size() > 0 && pointer[0] == '/' ? 1 : 0;
	while (true) {
		auto end = pointer.find('/', start);
		auto raw_segment = pointer.substr(start, end == string::npos ? string::npos : end - start);
		auto segment = DecodeJsonPointerSegment(raw_segment);
		result += ",";
		if (current && current->kind == JsonNodeKind::ARRAY && IsCanonicalArrayIndex(segment)) {
			result += segment;
			auto index = ParseIndex(segment);
			current = index < current->children.size() ? &current->children[index] : nullptr;
		} else {
			result += "\"" + QuackapiJsonEscape(segment) + "\"";
			current = current && current->kind == JsonNodeKind::OBJECT ? FindMember(*current, segment) : nullptr;
		}
		if (end == string::npos) {
			break;
		}
		start = end + 1;
	}
	return result + "]";
}

string QuackapiDuckdbTransformToOpenApi(const string &body_type) {
	JsonNode structure;
	if (!JsonReader(body_type).ParseDocument(structure)) {
		return "{}";
	}
	return TransformNodeToOpenApi(structure);
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
