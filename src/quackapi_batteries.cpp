#include "quackapi_server.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"

#include "quackapi_state.hpp"

namespace duckdb {

QuackapiLogLevel ParseQuackapiLogLevel(const string &raw) {
	auto lower = StringUtil::Lower(raw);
	StringUtil::Trim(lower);
	if (lower == "silent" || lower == "off" || lower == "none") {
		return QuackapiLogLevel::SILENT;
	}
	if (lower == "error") {
		return QuackapiLogLevel::ERROR_LEVEL;
	}
	if (lower == "warn" || lower == "warning") {
		return QuackapiLogLevel::WARN;
	}
	if (lower == "debug" || lower == "trace" || lower == "verbose") {
		return QuackapiLogLevel::DEBUG_LEVEL;
	}
	// Default + "info" / empty / unknown
	return QuackapiLogLevel::INFO;
}

void ProbeQuackapiRequestIdSource(DatabaseInstance &db, QuackapiServeOptions &opts) {
	// Request IDs are always core uuidv7 generated in C++ (see NextRequestId).
	// Never INSTALL/LOAD/SELECT tsid at serve or per request — that path added
	// a full SQL round-trip to every HTTP handler for no correctness gain.
	(void)db;
	opts.request_id_source = "uuidv7";
}

void RegisterQuackapiHealthRoutes(DatabaseInstance &db, const QuackapiServeOptions &opts) {
	auto &state = QuackapiState::Get(db);
	if (!opts.health_routes) {
		// Re-serve with health_routes:=false must drop prior auto-routes; a no-op
		// left __quackapi_health* in the registry (and still HTTP-reachable).
		state.DropRoute("__quackapi_health");
		state.DropRoute("__quackapi_healthz");
		return;
	}

	// Liveness: process is up and accepting HTTP. No auth. Listed in routes().
	QuackapiRoute health;
	health.name = "__quackapi_health";
	health.method = "GET";
	health.pattern = "/health";
	// Handler is also answered in C++ (object body); SQL is the registry source of truth.
	health.handler_sql = "SELECT 'ok' AS status";
	health.status = 200;
	state.AddRoute(health, true);

	// Readiness: DB handle usable + version + uptime (C++ enriches the body).
	QuackapiRoute healthz;
	healthz.name = "__quackapi_healthz";
	healthz.method = "GET";
	healthz.pattern = "/healthz";
	healthz.handler_sql = "SELECT 'ok' AS status, version() AS version";
	healthz.status = 200;
	state.AddRoute(healthz, true);
}

} // namespace duckdb
