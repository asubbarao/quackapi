#include "quackapi_request_binding.hpp"

#include <cstdlib>

#include "duckdb/common/string_util.hpp"
#include "yyjson.hpp"

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
JsonBodyParseResult ParseJsonBodyFields(const string &raw_body, case_insensitive_map_t<JsonBodyField> &fields) {
	fields.clear();
	if (raw_body.empty()) {
		return JsonBodyParseResult::INVALID_JSON;
	}
	auto document =
	    duckdb_yyjson::yyjson_read(raw_body.data(), raw_body.size(), duckdb_yyjson::YYJSON_READ_BIGNUM_AS_RAW);
	if (!document) {
		return JsonBodyParseResult::INVALID_JSON;
	}
	unique_ptr<duckdb_yyjson::yyjson_doc, void (*)(duckdb_yyjson::yyjson_doc *)> doc(document,
	                                                                                 duckdb_yyjson::yyjson_doc_free);
	auto root = duckdb_yyjson::yyjson_doc_get_root(doc.get());
	if (!root || !duckdb_yyjson::yyjson_is_obj(root)) {
		return JsonBodyParseResult::NOT_OBJECT;
	}
	auto iter = duckdb_yyjson::yyjson_obj_iter_with(root);
	duckdb_yyjson::yyjson_val *key = nullptr;
	while ((key = duckdb_yyjson::yyjson_obj_iter_next(&iter))) {
		auto value = duckdb_yyjson::yyjson_obj_iter_get_val(key);
		string name(duckdb_yyjson::yyjson_get_str(key), duckdb_yyjson::yyjson_get_len(key));
		if (duckdb_yyjson::yyjson_is_null(value)) {
			fields[name] = {string(), true};
		} else if (duckdb_yyjson::yyjson_is_str(value)) {
			fields[name] = {string(duckdb_yyjson::yyjson_get_str(value), duckdb_yyjson::yyjson_get_len(value)), false};
		} else {
			size_t length = 0;
			auto serialized = duckdb_yyjson::yyjson_val_write(value, duckdb_yyjson::YYJSON_WRITE_NOFLAG, &length);
			if (!serialized) {
				fields.clear();
				return JsonBodyParseResult::INVALID_JSON;
			}
			fields[name] = {string(serialized, length), false};
			std::free(serialized);
		}
	}
	return JsonBodyParseResult::SUCCESS;
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
