#include "quackapi_post_result.hpp"

#include "duckdb/common/types/value.hpp"

namespace duckdb {

LogicalType QuackapiHttpFetchResultType() {
	child_list_t<LogicalType> children;
	children.emplace_back("status", LogicalType::INTEGER);
	children.emplace_back("reason", LogicalType::VARCHAR);
	children.emplace_back("body", LogicalType::VARCHAR);
	children.emplace_back("headers", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
	children.emplace_back("error", LogicalType::VARCHAR);
	children.emplace_back("reused_connection", LogicalType::BOOLEAN);
	return LogicalType::STRUCT(std::move(children));
}

Value QuackapiHttpFetchResultValue(const QuackapiHttpFetchResult &result) {
	vector<Value> header_keys;
	vector<Value> header_values;
	for (auto &entry : result.headers) {
		header_keys.emplace_back(entry.first);
		header_values.emplace_back(entry.second);
	}
	child_list_t<Value> children;
	children.emplace_back("status", Value::INTEGER(static_cast<int32_t>(result.status)));
	children.emplace_back("reason", Value(result.reason));
	children.emplace_back("body", Value(result.body));
	children.emplace_back("headers", Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(header_keys),
	                                            std::move(header_values)));
	children.emplace_back("error",
	                      result.request_error.empty() ? Value(LogicalType::VARCHAR) : Value(result.request_error));
	children.emplace_back("reused_connection", Value::BOOLEAN(result.reused_connection));
	return Value::STRUCT(std::move(children));
}

unordered_map<string, string> QuackapiHttpHeadersFromValue(const Value &value) {
	unordered_map<string, string> out;
	if (value.IsNull()) {
		return out;
	}
	for (auto &entry : MapValue::GetChildren(value)) {
		auto &kv = StructValue::GetChildren(entry);
		if (kv.size() != 2 || kv[0].IsNull()) {
			continue;
		}
		out[kv[0].ToString()] = kv[1].IsNull() ? string() : kv[1].ToString();
	}
	return out;
}

} // namespace duckdb
