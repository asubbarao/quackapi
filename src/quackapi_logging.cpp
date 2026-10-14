#include "quackapi_logging.hpp"

#include <cstdio>

#include "quackapi_server.hpp"
#include "quackapi_util.hpp"

namespace duckdb {
string JsonEscape(const string &value) {
	return QuackapiJsonEscape(value);
}

void EmitAccessLogStderr(const QuackapiRequestRecord &entry) {
	string path_esc;
	path_esc.reserve(entry.path.size() + 8);
	for (unsigned char c : entry.path) {
		if (c == '"' || c == '\\') {
			path_esc += '\\';
			path_esc += static_cast<char>(c);
		} else if (c < 0x20) {
			char buf[8];
			snprintf(buf, sizeof(buf), "\\u%04x", c);
			path_esc += buf;
		} else {
			path_esc += static_cast<char>(c);
		}
	}
	fprintf(stderr,
	        "{\"type\":\"access\",\"method\":\"%s\",\"path\":\"%s\",\"status\":%d,"
	        "\"latency_ms\":%.3f,\"request_id\":\"%s\",\"bytes\":%llu,\"client_ip\":\"%s\","
	        "\"user_agent\":\"%s\",\"route_path\":\"%s\",\"trace_id\":\"%s\"}\n",
	        JsonEscape(entry.method).c_str(), path_esc.c_str(), entry.status, entry.duration_ms,
	        JsonEscape(entry.request_id).c_str(), (unsigned long long)entry.bytes_out,
	        JsonEscape(entry.client_ip).c_str(), JsonEscape(entry.user_agent).c_str(),
	        JsonEscape(entry.route_path).c_str(), JsonEscape(entry.trace_id).c_str());
}

} // namespace duckdb
