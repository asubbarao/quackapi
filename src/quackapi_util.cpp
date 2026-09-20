#include "quackapi_util.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"

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

QuackapiTokenStatement QuackapiStatementFromTokens(const vector<SimpleToken> &tokens) {
	QuackapiTokenStatement result;
	idx_t index = 0;
	bool glue_next = false;
	for (; index < tokens.size(); index++) {
		auto &token = tokens[index];
		if (token.type == TokenType::TERMINATOR || token.type == TokenType::END_OF_INPUT ||
		    token.type == TokenType::END_OF_INPUT_AUTOCOMPLETE) {
			break;
		}
		if (token.type == TokenType::COMMENT) {
			continue;
		}
		if (!result.query.empty() && !glue_next) {
			result.query += " ";
		}
		result.query += token.text;
		glue_next = token.type == TokenType::OPERATOR && token.text == "$";
	}
	if (index < tokens.size()) {
		// The statement owns its terminator, as a TopLevelStatement does.
		index++;
	}
	result.consumed_tokens = NumericCast<int64_t>(index);
	return result;
}

ParserExtensionParseResult QuackapiClaimTokens(ParserExtensionParseResult result, int64_t consumed_tokens) {
	switch (result.type) {
	case ParserExtensionResultType::PARSE_SUCCESSFUL:
		result.consumed_tokens = consumed_tokens;
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
