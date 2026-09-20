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

//! Advance past whitespace and SQL comments starting at s[pos]: repeated runs of
//! whitespace, '--' to end of line, and nestable '/* ... */' are all skipped.
//! Quote characters inside a comment (an apostrophe in a '--' comment, for
//! instance) are not special — a line comment always ends at the next newline
//! and a block comment always ends at its matching '*/', regardless of content.
static idx_t QuackapiSkipComments(const string &s, idx_t pos) {
	for (;;) {
		while (pos < s.size() && StringUtil::CharacterIsSpace(s[pos])) {
			pos++;
		}
		if (pos + 1 < s.size() && s[pos] == '-' && s[pos + 1] == '-') {
			pos += 2;
			while (pos < s.size() && s[pos] != '\n') {
				pos++;
			}
			continue;
		}
		if (pos + 1 < s.size() && s[pos] == '/' && s[pos + 1] == '*') {
			idx_t depth = 1;
			pos += 2;
			while (pos < s.size() && depth > 0) {
				if (pos + 1 < s.size() && s[pos] == '/' && s[pos + 1] == '*') {
					depth++;
					pos += 2;
				} else if (pos + 1 < s.size() && s[pos] == '*' && s[pos + 1] == '/') {
					depth--;
					pos += 2;
				} else {
					pos++;
				}
			}
			continue;
		}
		break;
	}
	return pos;
}

string QuackapiDdlTrim(const string &input) {
	return QuackapiTrim(input.substr(QuackapiSkipComments(input, 0)));
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
