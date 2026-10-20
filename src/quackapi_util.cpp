#include "quackapi_util.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/parser/parser.hpp"

#include <cstdio>

namespace duckdb {

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
