#include "quackapi_logging.hpp"

#include <string>

#include "duckdb/common/string_util.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logger.hpp"
#include "quackapi_server.hpp"
#include "quackapi_util.hpp"

namespace duckdb {
namespace {

string JsonString(const string &value) {
	return "\"" + QuackapiJsonEscape(value) + "\"";
}

string JsonOptional(const string &value) {
	return value.empty() ? "null" : JsonString(value);
}

string JsonOptionalDouble(double value) {
	return value < 0 ? "null" : StringUtil::Format("%.3f", value);
}

string JsonOptionalInteger(int64_t value) {
	return value < 0 ? "null" : std::to_string(value);
}

string BuildAccessLogJson(const QuackapiRequestRecord &entry) {
	string result = StringUtil::Format(
	    "{\"type\":\"access\",\"request_id\":%s,\"received_at_micros\":%lld,\"method\":%s,\"path\":%s,"
	    "\"route_name\":%s,\"route_path\":%s,\"status\":%d,\"duration_ms\":%.3f,"
	    "\"sql_prepare_ms\":%s,\"sql_execute_ms\":%s,\"rows_out\":%s,\"bytes_in\":%lld,\"bytes_out\":%lld,"
	    "\"client_ip\":%s,\"user_agent\":%s,\"http_version\":%s,\"error_type\":%s,\"error_message\":%s,"
	    "\"trace_id\":%s,\"span_id\":%s,\"parent_span_id\":%s,\"sampled\":%s,\"headers\":",
	    JsonString(entry.request_id), static_cast<long long>(entry.received_at_micros), JsonString(entry.method),
	    JsonString(entry.path), JsonOptional(entry.route_name), JsonOptional(entry.route_path), entry.status,
	    entry.duration_ms, JsonOptionalDouble(entry.sql_prepare_ms), JsonOptionalDouble(entry.sql_execute_ms),
	    JsonOptionalInteger(entry.rows_out), static_cast<long long>(entry.bytes_in),
	    static_cast<long long>(entry.bytes_out), JsonString(entry.client_ip), JsonString(entry.user_agent),
	    JsonOptional(entry.http_version), JsonOptional(entry.error_type), JsonOptional(entry.error_message),
	    JsonOptional(entry.trace_id), JsonOptional(entry.span_id), JsonOptional(entry.parent_span_id),
	    entry.sampled ? "true" : "false");
	if (!entry.headers_logged) {
		result += "null";
	} else {
		result += "{";
		bool first = true;
		for (auto &header : entry.headers) {
			if (!first) {
				result += ",";
			}
			first = false;
			result += JsonString(header.first) + ":" + JsonString(header.second);
		}
		result += "}";
	}
	result += ",\"query\":";
	result += entry.query_logged ? JsonString(entry.query) : "null";
	result += "}";
	return result;
}

} // namespace

void EmitQuackapiAccessLog(DatabaseInstance &db, const QuackapiRequestRecord &entry, LogLevel level) {
	auto &logger = Logger::Get(db);
	if (!logger.ShouldLog(DefaultLogType::NAME, level)) {
		return;
	}
	logger.WriteLog(DefaultLogType::NAME, level, BuildAccessLogJson(entry));
}

} // namespace duckdb
