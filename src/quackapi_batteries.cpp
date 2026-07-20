#include "quackapi_server.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/query_result.hpp"

#include "quackapi_state.hpp"

namespace duckdb {

QuackapiLogLevel ParseQuackapiLogLevel(const string &raw) {
	auto lower = StringUtil::Lower(raw);
	StringUtil::Trim(lower);
	if (lower == "silent" || lower == "off" || lower == "none") {
		return QuackapiLogLevel::SILENT;
	}
	if (lower == "error") {
		return QuackapiLogLevel::ERROR;
	}
	if (lower == "warn" || lower == "warning") {
		return QuackapiLogLevel::WARN;
	}
	if (lower == "debug" || lower == "trace" || lower == "verbose") {
		return QuackapiLogLevel::DEBUG;
	}
	// Default + "info" / empty / unknown
	return QuackapiLogLevel::INFO;
}

static const char *LogLevelDuckDBName(QuackapiLogLevel level) {
	switch (level) {
	case QuackapiLogLevel::SILENT:
	case QuackapiLogLevel::ERROR:
		return "ERROR";
	case QuackapiLogLevel::WARN:
		return "WARNING";
	case QuackapiLogLevel::DEBUG:
		return "DEBUG";
	case QuackapiLogLevel::INFO:
	default:
		return "INFO";
	}
}

//! Safe serve default when nothing was configured (valsafe HARDENING P1-2).
static constexpr const char *SERVE_DEFAULT_MEMORY_LIMIT = "256MB";

static bool IsAtSystemDefaultMemoryLimit(DatabaseInstance &db) {
	auto &config = DBConfig::GetConfig(db);
	if (!config.file_system) {
		return false;
	}
	auto available = DBConfig::GetSystemAvailableMemory(*config.file_system);
	idx_t system_default;
	if (available == DBConfigOptions().maximum_memory) {
		system_default = available;
	} else {
		// Match DBConfig::SetDefaultMaxMemory().
		system_default = available * 8 / 10;
	}
	return config.options.maximum_memory == system_default;
}

static bool RunSet(Connection &con, const string &sql, string &err_out) {
	auto res = con.Query(sql);
	if (res->HasError()) {
		err_out = res->GetError();
		return false;
	}
	return true;
}

string ApplyQuackapiServerDefaults(ClientContext &context, QuackapiServeOptions &opts) {
	// Correct-by-default SETs/PRAGMAs for a long-lived HTTP server process.
	// Each SET is documented (WHY) and overridable via serve() named params.
	// NEVER disables safety (no allow_unsigned_extensions, no disabled checks).
	vector<string> applied;
	Connection con(*context.db);
	string err;

	// --- memory_limit ---
	// WHY: unbounded RAM is the #1 footgun for multi-tenant HTTP handlers; a
	// conservative ceiling prevents a single query from OOMing the host.
	// Non-clobber: never overwrite an operator-set DuckDB memory_limit.
	{
		string limit_to_apply;
		if (!opts.memory_limit.empty()) {
			limit_to_apply = opts.memory_limit;
		} else if (IsAtSystemDefaultMemoryLimit(*context.db)) {
			limit_to_apply = SERVE_DEFAULT_MEMORY_LIMIT;
		}
		if (!limit_to_apply.empty()) {
			try {
				DBConfig::ParseMemoryLimit(limit_to_apply);
			} catch (std::exception &ex) {
				throw InvalidInputException("quackapi_serve: invalid memory_limit '%s': %s", limit_to_apply,
				                            ex.what());
			}
			auto escaped = StringUtil::Replace(limit_to_apply, "'", "''");
			if (RunSet(con, StringUtil::Format("SET memory_limit TO '%s'", escaped), err)) {
				applied.push_back(StringUtil::Format(
				    "memory_limit=%s (WHY: RAM guardrail for multi-query HTTP workers; "
				    "prevents one handler from OOMing the process)",
				    limit_to_apply));
			} else {
				fprintf(stderr, "quackapi: could not SET memory_limit: %s\n", err.c_str());
			}
		} else {
			applied.push_back("memory_limit=<operator/prior> (WHY: non-clobber; left alone)");
		}
	}

	// --- preserve_insertion_order ---
	// WHY: insertion-order preservation serializes some pipelines; servers almost
	// never need row order unless ORDER BY is present — false raises throughput.
	{
		const char *pio = opts.preserve_insertion_order ? "true" : "false";
		if (RunSet(con, StringUtil::Format("SET preserve_insertion_order = %s", pio), err)) {
			applied.push_back(StringUtil::Format(
			    "preserve_insertion_order=%s (WHY: %s)", pio,
			    opts.preserve_insertion_order
			        ? "operator requested stable scan order"
			        : "server throughput — allow reordering when no ORDER BY"));
		} else {
			fprintf(stderr, "quackapi: could not SET preserve_insertion_order: %s\n", err.c_str());
		}
	}

	// --- enable_http_metadata_cache ---
	// WHY: outbound HTTP (httpfs / curl_httpfs companions) reuses ETag /
	// Last-Modified; cuts origin load for repeated remote reads from handlers.
	// (enable_object_cache is a DuckDB no-op placeholder — intentionally skipped.)
	{
		const char *v = opts.enable_http_metadata_cache ? "true" : "false";
		if (RunSet(con, StringUtil::Format("SET enable_http_metadata_cache = %s", v), err)) {
			applied.push_back(StringUtil::Format(
			    "enable_http_metadata_cache=%s (WHY: cache HTTP ETag/Last-Modified for "
			    "outbound companion fetches)",
			    v));
		} else {
			fprintf(stderr, "quackapi: could not SET enable_http_metadata_cache: %s\n", err.c_str());
		}
	}

	// --- threads ---
	// WHY: default leaves DuckDB at all-cores (correct for a dedicated server).
	// Override only when the operator passes threads:= so multi-tenant hosts can
	// cap CPU without editing duckdb config files.
	if (!opts.threads.empty()) {
		auto escaped = StringUtil::Replace(opts.threads, "'", "''");
		// Accept bare int or quoted.
		string sql;
		bool all_digit = !opts.threads.empty();
		for (char c : opts.threads) {
			if (c < '0' || c > '9') {
				all_digit = false;
				break;
			}
		}
		if (all_digit) {
			sql = StringUtil::Format("SET threads = %s", opts.threads);
		} else {
			sql = StringUtil::Format("SET threads TO '%s'", escaped);
		}
		if (RunSet(con, sql, err)) {
			applied.push_back(StringUtil::Format(
			    "threads=%s (WHY: operator-capped worker pool for multi-tenant hosts)", opts.threads));
		} else {
			fprintf(stderr, "quackapi: could not SET threads: %s\n", err.c_str());
		}
	} else {
		applied.push_back("threads=<DuckDB default=all cores> (WHY: max parallel query work for server)");
	}

	// --- DuckDB built-in logging (ALL ON by default) ---
	// WHY: without a logger, production incidents have no query/error trail.
	// enable_logging + INFO + stdout captures QueryLog and errors for the process.
	// logging_storage=stdout so a supervised server process pipes logs to the host.
	if (opts.enable_logging && opts.log_level != QuackapiLogLevel::SILENT) {
		const char *level = LogLevelDuckDBName(opts.log_level);
		// Prefer CALL enable_logging (current DuckDB API) — sets storage + level.
		auto call = con.Query(StringUtil::Format(
		    "CALL enable_logging(level:='%s', storage:='stdout')", level));
		if (call->HasError()) {
			// Fall back to SET surface if CALL signature differs.
			RunSet(con, "SET enable_logging = true", err);
			RunSet(con, StringUtil::Format("SET logging_level = '%s'", level), err);
			RunSet(con, "SET logging_storage = 'stdout'", err);
			applied.push_back(StringUtil::Format(
			    "enable_logging=true logging_level=%s logging_storage=stdout "
			    "(WHY: built-in query/error capture for server ops; CALL failed: %s)",
			    level, call->GetError()));
		} else {
			applied.push_back(StringUtil::Format(
			    "CALL enable_logging(level:='%s', storage:='stdout') "
			    "(WHY: DuckDB built-in logger ON — queries + errors to stdout)",
			    level));
		}
		// Deprecated but still present: keep HTTP client logging on (already true
		// by default on v1.5.4 — re-assert for older forks).
		if (RunSet(con, "SET enable_http_logging = true", err)) {
			applied.push_back(
			    "enable_http_logging=true (WHY: outbound HTTP client request log; "
			    "deprecated DuckDB setting, still effective)");
		}
	} else {
		applied.push_back("enable_logging=<skipped> (WHY: operator disabled or log_level=silent)");
	}

	// --- Compose quack transport log if the option exists ---
	// WHY: when duckdb-quack is loaded, share one log surface with REST.
	{
		auto &config = DBConfig::GetConfig(*context.db);
		Value existing;
		if (config.TryGetCurrentSetting("quack_log_level", existing) ||
		    config.TryGetCurrentSetting("quack_logging", existing)) {
			// Best-effort: set to a verbose-enough value when present.
			if (config.TryGetCurrentSetting("quack_log_level", existing)) {
				RunSet(con, "SET quack_log_level = 'info'", err);
				applied.push_back("quack_log_level=info (WHY: wire quack transport log when present)");
			}
			if (config.TryGetCurrentSetting("quack_logging", existing)) {
				RunSet(con, "SET quack_logging = true", err);
				applied.push_back("quack_logging=true (WHY: wire quack transport log when present)");
			}
		} else {
			applied.push_back("quack transport log=<n/a> (WHY: quack not loaded; no settings to wire)");
		}
	}

	// Transport knobs are applied in QuackapiHttpServer ctor (httplib).
	applied.push_back(StringUtil::Format(
	    "http keep_alive_max_count=%d keep_alive_timeout_sec=%d "
	    "(WHY: connection reuse cuts TCP/TLS handshake cost)",
	    opts.keep_alive_max_count, opts.keep_alive_timeout_sec));
	applied.push_back(StringUtil::Format(
	    "http read_timeout_sec=%d write_timeout_sec=%d "
	    "(WHY: bound stalled clients so workers are not pinned forever)",
	    opts.read_timeout_sec, opts.write_timeout_sec));
	applied.push_back(StringUtil::Format(
	    "http worker_threads=%d (WHY: concurrent request handlers; cap prevents "
	    "unbounded thread spawn under load)",
	    opts.worker_threads));
	applied.push_back(StringUtil::Format(
	    "http payload_max_length=%llu (WHY: body size DoS guard — 413 above cap)",
	    (unsigned long long)QUACKAPI_PAYLOAD_MAX_LENGTH));
	applied.push_back(StringUtil::Format(
	    "access_log=%s log_level=%s (WHY: every request → structured stderr line "
	    "for correlation with X-Request-ID)",
	    opts.access_log ? "true" : "false", LogLevelDuckDBName(opts.log_level)));
	applied.push_back(StringUtil::Format(
	    "health_routes=%s (WHY: /health liveness + /healthz readiness out of the box)",
	    opts.health_routes ? "true" : "false"));

	string summary = StringUtil::Join(applied, "\n");
	if (opts.log_level >= QuackapiLogLevel::INFO) {
		fprintf(stderr, "quackapi: server defaults applied:\n%s\n", summary.c_str());
	}
	return summary;
}

void ProbeQuackapiRequestIdSource(DatabaseInstance &db, QuackapiServeOptions &opts) {
	// Prefer community tsid() when it LOADs; else DuckDB core uuidv7().
	// Compose only — INSTALL FROM community is best-effort (may need network).
	Connection con(db);
	auto try_tsid = [&]() -> bool {
		auto res = con.Query("SELECT tsid()");
		return !res->HasError();
	};
	if (try_tsid()) {
		opts.request_id_source = "tsid";
		return;
	}
	auto load = con.Query("LOAD tsid");
	if (load->HasError()) {
		auto inst = con.Query("INSTALL tsid FROM community");
		if (!inst->HasError()) {
			load = con.Query("LOAD tsid");
		}
	}
	if (!load->HasError() && try_tsid()) {
		opts.request_id_source = "tsid";
		return;
	}
	// uuidv7 is core (v1.5+).
	opts.request_id_source = "uuidv7";
}

void RegisterQuackapiHealthRoutes(DatabaseInstance &db, const QuackapiServeOptions &opts) {
	if (!opts.health_routes) {
		return;
	}
	auto &state = QuackapiState::Get(db);

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
