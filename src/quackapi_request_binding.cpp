#include "quackapi_request_binding.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {
bool IsJsonMediaType(const string &media) {
	return media == "application/json" || StringUtil::EndsWith(media, "+json");
}

bool IsFormUrlEncodedMediaType(const string &media) {
	return media == "application/x-www-form-urlencoded";
}

bool IsMultipartMediaType(const string &media) {
	return StringUtil::StartsWith(media, "multipart/form-data");
}

bool IsBodyMethod(const string &method) {
	return method == "POST" || method == "PUT" || method == "PATCH";
}

//! application/x-www-form-urlencoded → key/value map (last wins).
void ParseFormUrlEncoded(const string &body, case_insensitive_map_t<string> &out) {
	idx_t i = 0;
	while (i < body.size()) {
		idx_t amp = body.find('&', i);
		if (amp == string::npos) {
			amp = body.size();
		}
		string pair = body.substr(i, amp - i);
		if (!pair.empty()) {
			auto eq = pair.find('=');
			string key, val;
			if (eq == string::npos) {
				key = duckdb_httplib::decode_query_component(pair, true);
			} else {
				key = duckdb_httplib::decode_query_component(pair.substr(0, eq), true);
				val = duckdb_httplib::decode_query_component(pair.substr(eq + 1), true);
			}
			if (!key.empty()) {
				out[key] = val;
			}
		}
		i = amp + 1;
	}
}
//! Fast path: flat JSON object of string/number/bool/null scalars (no DuckDB round-trip).
//! Returns true on success. false → caller may fall back to SQL extract or error.
bool TryExtractFlatJsonObject(const string &raw_body, case_insensitive_map_t<JsonBodyField> &fields) {
	const char *s = raw_body.c_str();
	const char *end = s + raw_body.size();
	auto skip_ws = [&]() {
		while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) {
			s++;
		}
	};
	auto is_digit = [](char c) {
		return c >= '0' && c <= '9';
	};
	// This path deliberately accepts only unescaped JSON strings.  Escapes require
	// full JSON decoding (in particular, UTF-16 surrogate handling), so leave them
	// to DuckDB instead of attempting a partial implementation here.
	auto parse_plain_string = [&](string &out) {
		if (s >= end || *s != '"') {
			return false;
		}
		s++;
		while (s < end && *s != '"') {
			const auto ch = static_cast<unsigned char>(*s);
			if (ch < 0x20 || *s == '\\') {
				return false;
			}
			out.push_back(*s++);
		}
		if (s >= end) {
			return false;
		}
		s++;
		return true;
	};
	auto parse_number = [&]() {
		const char *start = s;
		if (s < end && *s == '-') {
			s++;
		}
		if (s >= end) {
			return false;
		}
		if (*s == '0') {
			s++;
			if (s < end && is_digit(*s)) {
				return false;
			}
		} else if (*s >= '1' && *s <= '9') {
			do {
				s++;
			} while (s < end && is_digit(*s));
		} else {
			return false;
		}
		if (s < end && *s == '.') {
			s++;
			if (s >= end || !is_digit(*s)) {
				return false;
			}
			do {
				s++;
			} while (s < end && is_digit(*s));
		}
		if (s < end && (*s == 'e' || *s == 'E')) {
			s++;
			if (s < end && (*s == '+' || *s == '-')) {
				s++;
			}
			if (s >= end || !is_digit(*s)) {
				return false;
			}
			do {
				s++;
			} while (s < end && is_digit(*s));
		}
		return s > start;
	};
	skip_ws();
	if (s >= end || *s != '{') {
		return false;
	}
	s++;
	fields.clear();
	skip_ws();
	if (s < end && *s == '}') {
		s++;
		skip_ws();
		return s == end;
	}
	while (s < end) {
		skip_ws();
		string key;
		if (!parse_plain_string(key)) {
			return false;
		}
		skip_ws();
		if (s >= end || *s != ':') {
			return false;
		}
		s++;
		skip_ws();
		if (s >= end) {
			return false;
		}
		string val;
		bool explicit_null = false;
		if (*s == '"') {
			if (!parse_plain_string(val)) {
				return false;
			}
		} else if (*s == 'n' && s + 4 <= end && string(s, 4) == "null") {
			explicit_null = true;
			s += 4;
		} else if (*s == 't' && s + 4 <= end && string(s, 4) == "true") {
			val = "true";
			s += 4;
		} else if (*s == 'f' && s + 5 <= end && string(s, 5) == "false") {
			val = "false";
			s += 5;
		} else if (*s == '-' || (*s >= '0' && *s <= '9')) {
			const char *start = s;
			if (!parse_number()) {
				return false;
			}
			val.assign(start, s - start);
		} else {
			// nested object/array — not flat; fall back
			return false;
		}
		fields[key] = {val, explicit_null};
		skip_ws();
		if (s < end && *s == ',') {
			s++;
			continue;
		}
		if (s < end && *s == '}') {
			s++;
			skip_ws();
			return s == end;
		}
		return false;
	}
	return false;
}
//! Parse application/x-www-form-urlencoded query into httplib Params.
void ParseQueryStringIntoParams(const string &query, duckdb_httplib::Params &params) {
	if (query.empty()) {
		return;
	}
	idx_t start = 0;
	while (start < query.size()) {
		idx_t amp = query.find('&', start);
		if (amp == string::npos) {
			amp = query.size();
		}
		string pair = query.substr(start, amp - start);
		start = amp + 1;
		if (pair.empty()) {
			continue;
		}
		auto eq = pair.find('=');
		if (eq == string::npos) {
			params.emplace(duckdb_httplib::decode_query_component(pair, true), string());
		} else {
			params.emplace(duckdb_httplib::decode_query_component(pair.substr(0, eq), true),
			               duckdb_httplib::decode_query_component(pair.substr(eq + 1), true));
		}
	}
}

} // namespace duckdb
