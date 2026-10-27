#include "quackapi_util.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/write_stream.hpp"
#include "duckdb/parser/parser.hpp"

#include <cstdio>

namespace duckdb {

namespace {

LogicalType TypedColumnsType() {
	child_list_t<LogicalType> fields;
	fields.emplace_back("name", LogicalType::VARCHAR);
	fields.emplace_back("type", LogicalType::TYPE());
	return LogicalType::LIST(LogicalType::STRUCT(std::move(fields)));
}

Value TypedResultForBudget(const Value &rows, const Value &columns) {
	child_list_t<LogicalType> fields;
	fields.emplace_back("columns", TypedColumnsType());
	fields.emplace_back("rows", LogicalType::LIST(LogicalType::VARIANT()));
	return Value::STRUCT(LogicalType::STRUCT(std::move(fields)), vector<Value> {columns, rows});
}

class TypedRowsCountStream : public WriteStream {
public:
	idx_t bytes = 0;
	void WriteData(const_data_ptr_t, idx_t size) override {
		const auto max_value = NumericLimits<idx_t>::Maximum();
		bytes = size > max_value - bytes ? max_value : bytes + size;
	}
};

Value TypedColumnsValue(const LogicalType &row_type) {
	auto column_type = ListType::GetChildType(TypedColumnsType());
	vector<Value> column_values;
	const auto &fields = StructType::GetChildTypes(row_type);
	column_values.reserve(fields.size());
	for (const auto &column : fields) {
		column_values.emplace_back(Value::STRUCT(
		    column_type, vector<Value> {Value(column.first.GetIdentifierName()), Value::TYPE(column.second)}));
	}
	return Value::LIST(column_type, std::move(column_values));
}

idx_t NativeSerializedBytes(const Value &value) {
	TypedRowsCountStream counter;
	BinarySerializer::Serialize(value, counter);
	return counter.bytes;
}

idx_t SaturatingAdd(idx_t lhs, idx_t rhs) {
	const auto max_value = NumericLimits<idx_t>::Maximum();
	return rhs > max_value - lhs ? max_value : lhs + rhs;
}

} // namespace

QuackapiTypedResult::QuackapiTypedResult()
    : rows(Value(LogicalType::LIST(LogicalType::VARIANT()))), columns(Value(TypedColumnsType())) {
}

QuackapiTypedRowCollector::QuackapiTypedRowCollector(vector<string> names_p, vector<LogicalType> types_p,
                                                     idx_t max_bytes_p)
    : column_count(names_p.size()), max_bytes(max_bytes_p) {
	if (column_count != types_p.size()) {
		throw InvalidInputException("typed row names and types have different sizes");
	}
	QueryResult::DeduplicateColumns(names_p);
	child_list_t<LogicalType> children;
	for (idx_t i = 0; i < column_count; i++) {
		children.emplace_back(std::move(names_p[i]), std::move(types_p[i]));
	}
	row_type = LogicalType::STRUCT(std::move(children));
	auto empty_rows = Value::LIST(LogicalType::VARIANT(), vector<Value>());
	empty_rows_bytes = NativeSerializedBytes(empty_rows);
	empty_result_bytes = NativeSerializedBytes(TypedResultForBudget(empty_rows, TypedColumnsValue(row_type)));
}

void QuackapiTypedRowCollector::AppendRow(vector<Value> values) {
	if (values.size() != column_count) {
		throw InvalidInputException("typed row has %s values for %s columns", to_string(values.size()),
		                            to_string(column_count));
	}
	auto row = Value::STRUCT(row_type, std::move(values)).DefaultCastAs(LogicalType::VARIANT());
	// The full response is a fixed schema wrapper around LIST<VARIANT>. Native serialization
	// of a singleton list minus an empty list gives this row's exact additive payload: both
	// list headers encode counts 1 and 0 in one byte. Adjust the wrapper baseline only for the
	// list count varint width, measured through the same native codec.
	auto singleton_rows = Value::LIST(LogicalType::VARIANT(), vector<Value> {row});
	auto singleton_bytes = NativeSerializedBytes(singleton_rows);
	if (singleton_bytes < empty_rows_bytes) {
		throw InternalException("native typed row serialization produced a negative row size");
	}
	auto row_delta = singleton_bytes - empty_rows_bytes;
	auto next_count = rows.size() + 1;
	auto zero_count_bytes = NativeSerializedBytes(Value::UBIGINT(0));
	auto next_count_bytes = NativeSerializedBytes(Value::UBIGINT(NumericCast<uint64_t>(next_count)));
	if (next_count_bytes < zero_count_bytes) {
		throw InternalException("native typed list count serialization decreased in size");
	}
	auto predicted_bytes = SaturatingAdd(empty_result_bytes, row_payload_bytes);
	predicted_bytes = SaturatingAdd(predicted_bytes, row_delta);
	predicted_bytes = SaturatingAdd(predicted_bytes, next_count_bytes - zero_count_bytes);
	if (predicted_bytes > max_bytes) {
		throw QuackapiTypedRowsTooLarge("native typed rows and schema exceed quackapi_max_response_bytes");
	}
	row_payload_bytes = SaturatingAdd(row_payload_bytes, row_delta);
	rows.emplace_back(std::move(row));
}

void QuackapiTypedRowCollector::AppendChunk(const DataChunk &chunk) {
	if (chunk.ColumnCount() != column_count) {
		throw InvalidInputException("typed row chunk does not match the result schema");
	}
	for (idx_t row = 0; row < chunk.size(); row++) {
		vector<Value> values;
		values.reserve(chunk.ColumnCount());
		for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
			values.emplace_back(chunk.GetValue(col, row));
		}
		AppendRow(std::move(values));
	}
}

QuackapiTypedResult QuackapiTypedRowCollector::Finish() {
	QuackapiTypedResult result;
	result.rows = Value::LIST(LogicalType::VARIANT(), std::move(rows));
	result.columns = TypedColumnsValue(row_type);
	if (NativeSerializedBytes(TypedResultForBudget(result.rows, result.columns)) > max_bytes) {
		throw QuackapiTypedRowsTooLarge("native typed rows and schema exceed quackapi_max_response_bytes");
	}
	return result;
}

QuackapiTypedResult QuackapiTypedRowsFromResult(QueryResult &result, idx_t max_bytes) {
	QuackapiTypedRowCollector collector(QuackapiNames(result.GetNames()), result.GetTypes(), max_bytes);
	while (auto chunk = result.Fetch()) {
		collector.AppendChunk(*chunk);
	}
	return collector.Finish();
}

string QuackapiTrim(const string &input) {
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

bool QuackapiExecRouteParam(const string &handler_sql, string &param_name) {
	auto handler = QuackapiTrim(handler_sql);
	if (handler.size() < 2 || handler[0] != '$') {
		return false;
	}
	auto is_start = [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
	};
	if (!is_start(handler[1])) {
		return false;
	}
	for (idx_t i = 2; i < handler.size(); i++) {
		if (!is_start(handler[i]) && !(handler[i] >= '0' && handler[i] <= '9')) {
			return false;
		}
	}
	auto name = handler.substr(1);
	if (StringUtil::StartsWith(StringUtil::Lower(name), "claims_")) {
		return false;
	}
	param_name = name;
	return true;
}

// No space where SQL writes none, so the clause grammar sees `f(x)`, `t.c`, `x::int`, `VARCHAR[]`, `$sql`.
static bool JoinsWithoutSpace(const string &previous, const string &next) {
	if (previous == "(" || previous == "[" || previous == "." || previous == "::" || previous == "$") {
		return true;
	}
	return next == ")" || next == "]" || next == "," || next == "." || next == "::";
}

static bool IsStatementEnd(TokenType type) {
	return type == TokenType::TERMINATOR || type == TokenType::END_OF_INPUT ||
	       type == TokenType::END_OF_INPUT_AUTOCOMPLETE;
}

bool QuackapiTokensToSql(const vector<SimpleToken> &tokens, idx_t &statement_tokens, string &sql, string &error) {
	sql.clear();
	vector<idx_t> starts;
	idx_t count = 0;
	while (count < tokens.size() && !IsStatementEnd(tokens[count].type)) {
		auto &token = tokens[count];
		if (count > 0) {
			auto &previous = tokens[count - 1];
			// A string after a string is concatenated only across a newline, which the token view does not
			// carry - rebuilding either way could change what the statement means.
			if (previous.type == TokenType::STRING_LITERAL && token.type == TokenType::STRING_LITERAL &&
			    StringUtil::StartsWith(token.text, "'")) {
				error = "quackapi cannot rebuild adjacent string literals " + previous.text + " " + token.text +
				        " - join them with || or put them in one literal";
				return false;
			}
			if (!JoinsWithoutSpace(previous.text, token.text)) {
				sql += " ";
			}
		}
		starts.push_back(sql.size());
		sql += token.text;
		count++;
	}
	// The rebuilt text must tokenize back into exactly these tokens, or a literal or operator moved.
	auto retokenized = Parser::Tokenize(sql);
	bool same = retokenized.size() == starts.size();
	for (idx_t i = 0; same && i < starts.size(); i++) {
		same = retokenized[i].start == starts[i];
	}
	if (!same) {
		error = "quackapi could not rebuild the statement from its tokens without changing them: " + sql;
		return false;
	}
	statement_tokens = count;
	return true;
}

ParserExtensionParseResult QuackapiParseTokens(ParserExtensionInfo *info, const vector<SimpleToken> &tokens,
                                               quackapi_string_parse_t parse) {
	idx_t statement_tokens = 0;
	string sql;
	string error;
	if (!QuackapiTokensToSql(tokens, statement_tokens, sql, error)) {
		// Only claim the failure if the statement is one of ours.
		auto probe = parse(info, sql);
		if (probe.type == ParserExtensionResultType::DISPLAY_ORIGINAL_ERROR) {
			return ParserExtensionParseResult();
		}
		ParserExtensionParseResult result(error);
		result.consumed_tokens = -1;
		return result;
	}
	auto result = parse(info, sql);
	switch (result.type) {
	case ParserExtensionResultType::PARSE_SUCCESSFUL:
		// claim the terminator too, as a statement does
		result.consumed_tokens = NumericCast<int64_t>(MinValue<idx_t>(statement_tokens + 1, tokens.size()));
		break;
	case ParserExtensionResultType::DISPLAY_EXTENSION_ERROR:
		result.consumed_tokens = -1;
		break;
	default:
		result.consumed_tokens = 0;
		break;
	}
	return result;
}

string QuackapiJsonEscape(const string &input) {
	string result;
	result.reserve(input.size() + 2);
	for (unsigned char c : input) {
		switch (c) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\b':
			result += "\\b";
			break;
		case '\f':
			result += "\\f";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				result += buf;
			} else {
				result += static_cast<char>(c);
			}
		}
	}
	return result;
}

} // namespace duckdb
