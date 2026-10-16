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
