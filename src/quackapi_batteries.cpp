#include "quackapi_server.hpp"
#include "quackapi_http_fetch.hpp"
#include "quackapi_imports.hpp"

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

const char *QuackapiLogLevelTokens() {
	return "silent, off, none, error, warn, warning, info, debug, trace, verbose";
}

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
		return QuackapiLogLevel::DEBUG_LEVEL;
	}
	if (lower.empty() || lower == "info") {
		return QuackapiLogLevel::INFO;
	}
	throw InvalidInputException("log_level must be one of [%s], not '%s'", QuackapiLogLevelTokens(), raw);
}

static const char *LogLevelDuckDBName(QuackapiLogLevel level) {
	switch (level) {
	case QuackapiLogLevel::SILENT:
	case QuackapiLogLevel::ERROR:
		return "ERROR";
	case QuackapiLogLevel::WARN:
		return "WARNING";
	case QuackapiLogLevel::DEBUG_LEVEL:
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

//! A SET the operator asked for by name. Serve returns a listen_url that is
//! taken as a statement of fact about the process configuration, so a knob that
//! could not be applied has to stop the serve rather than reach stderr.
static void RequireSet(Connection &con, const string &sql, const string &setting) {
	string err;
	if (!RunSet(con, sql, err)) {
		throw InvalidInputException("quackapi_serve: could not apply %s: %s", setting, err);
	}
}

string ApplyQuackapiServerDefaults(ClientContext &context, QuackapiServeOptions &opts) {
	// Correct-by-default SETs/PRAGMAs for a long-lived HTTP server process.
	// Each SET is documented (WHY) and overridable via serve() named params.
	// NEVER disables safety (no allow_unsigned_extensions, no disabled checks).
	vector<string> applied;
	// Build stamp so operators/agents can confirm the loaded extension matches the tree.
	applied.push_back(StringUtil::Format("quackapi_request_path=perf (enable_logging=%s access_log=%s) "
	                                     "(WHY: per-request Connection reset + uuidv7 ids)",
	                                     opts.enable_logging ? "true" : "false", opts.access_log ? "true" : "false"));
	Connection con(*context.db);
	string err;

	// --- memory_limit ---
	// WHY: unbounded RAM is the #1 footgun for multi-tenant HTTP handlers; a
	// conservative ceiling prevents a single query from OOMing the host.
	// Non-clobber: never overwrite an operator-set DuckDB memory_limit — and
	// never impose the serve default on an untuned instance, where the session
	// that called serve would inherit a 256MB ceiling it never asked for.
	{
		string limit_to_apply;
		if (!opts.memory_limit.empty()) {
			limit_to_apply = opts.memory_limit;
		} else if (opts.tune && IsAtSystemDefaultMemoryLimit(*context.db)) {
			limit_to_apply = SERVE_DEFAULT_MEMORY_LIMIT;
		}
		if (!limit_to_apply.empty()) {
			try {
				DBConfig::ParseMemoryLimit(limit_to_apply);
			} catch (std::exception &ex) {
				throw InvalidInputException("quackapi_serve: invalid memory_limit '%s': %s", limit_to_apply, ex.what());
			}
			auto escaped = StringUtil::Replace(limit_to_apply, "'", "''");
			RequireSet(con, StringUtil::Format("SET memory_limit TO '%s'", escaped), "memory_limit");
			applied.push_back(StringUtil::Format("memory_limit=%s (WHY: RAM guardrail for multi-query HTTP workers; "
			                                     "prevents one handler from OOMing the process)",
			                                     limit_to_apply));
		} else {
			applied.push_back("memory_limit=<operator/prior> (WHY: non-clobber; left alone)");
		}
	}

	// --- preserve_insertion_order ---
	// WHY: insertion-order preservation serializes some pipelines; servers almost
	// never need row order unless ORDER BY is present — false raises throughput.
	// It also changes what every ORDER BY-less query in the process returns, so
	// it moves only on an explicit ask.
	if (opts.preserve_insertion_order_set || opts.tune) {
		const char *pio = opts.preserve_insertion_order ? "true" : "false";
		RequireSet(con, StringUtil::Format("SET preserve_insertion_order = %s", pio), "preserve_insertion_order");
		applied.push_back(StringUtil::Format("preserve_insertion_order=%s (WHY: %s)", pio,
		                                     opts.preserve_insertion_order
		                                         ? "operator requested stable scan order"
		                                         : "server throughput — allow reordering when no ORDER BY"));
	} else {
		applied.push_back("preserve_insertion_order=<DuckDB default> (WHY: untuned serve leaves row-order "
		                  "semantics of the shared instance alone)");
	}

	// --- postgres attach OLTP: disable ctid parallel page scan ---
	// WHY: with pg_use_ctid_scan=true (DuckDB default), point lookups like
	//   SELECT … FROM pg.t WHERE id = $id
	// profile as POSTGRES_SCAN cumulative_rows_scanned ≈ whole table (~100k+),
	// not an index probe. false forces COPY (SELECT … WHERE id=…) so Postgres
	// uses the PK. Best-effort: the setting only exists once postgres is loaded,
	// so report which of the two happened instead of claiming it either way.
	if (opts.tune) {
		if (RunSet(con, "SET pg_use_ctid_scan = false", err)) {
			applied.push_back("pg_use_ctid_scan=false (WHY: ATTACH point lookups use PG index via "
			                  "WHERE pushdown, not full ctid range scan)");
		} else {
			applied.push_back("pg_use_ctid_scan=<unset> (WHY: postgres extension not loaded)");
		}
	}

	// --- enable_http_metadata_cache ---
	// WHY: outbound HTTP (httpfs / curl_httpfs companions) reuses ETag /
	// Last-Modified; cuts origin load for repeated remote reads from handlers.
	// (enable_object_cache is a DuckDB no-op placeholder — intentionally skipped.)
	if (opts.enable_http_metadata_cache_set || opts.tune) {
		const char *v = opts.enable_http_metadata_cache ? "true" : "false";
		RequireSet(con, StringUtil::Format("SET enable_http_metadata_cache = %s", v), "enable_http_metadata_cache");
		applied.push_back(StringUtil::Format("enable_http_metadata_cache=%s (WHY: cache HTTP ETag/Last-Modified for "
		                                     "outbound companion fetches)",
		                                     v));
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
		RequireSet(con, sql, "threads");
		applied.push_back(StringUtil::Format("threads=%s (WHY: operator-capped worker pool for multi-tenant hosts)",
		                                     opts.threads));
	} else {
		applied.push_back("threads=<DuckDB default=all cores> (WHY: max parallel query work for server)");
	}

	// --- DuckDB built-in logging (only on an explicit ask) ---
	// WHY: QueryLog-per-handler-SQL to stdout is a multi-ms tax under load and
	// serializes workers. HTTP ops use access_log (structured stderr). Opt in
	// with enable_logging:=true when debugging query plans / errors. Omitting it
	// leaves whatever logging the process already had — forcing it off here
	// silenced loggers that had nothing to do with quackapi.
	if (!opts.enable_logging_set && !opts.tune) {
		applied.push_back("enable_logging=<DuckDB default> (WHY: untuned serve leaves the instance logger alone; "
		                  "pass enable_logging:=true/false to move it)");
	} else if (opts.enable_logging && opts.log_level != QuackapiLogLevel::SILENT) {
		const char *level = LogLevelDuckDBName(opts.log_level);
		// Prefer CALL enable_logging (current DuckDB API) — sets storage + level.
		auto call = con.Query(StringUtil::Format("CALL enable_logging(level:='%s', storage:='stdout')", level));
		if (call->HasError()) {
			// Fall back to SET surface if CALL signature differs.
			RunSet(con, "SET enable_logging = true", err);
			RunSet(con, StringUtil::Format("SET logging_level = '%s'", level), err);
			RunSet(con, "SET logging_storage = 'stdout'", err);
			applied.push_back(StringUtil::Format("enable_logging=true logging_level=%s logging_storage=stdout "
			                                     "(WHY: built-in query/error capture for server ops; CALL failed: %s)",
			                                     level, call->GetError()));
		} else {
			applied.push_back(StringUtil::Format("CALL enable_logging(level:='%s', storage:='stdout') "
			                                     "(WHY: DuckDB built-in logger ON — queries + errors to stdout)",
			                                     level));
		}
		// Deprecated but still present: keep HTTP client logging on (already true
		// by default on v1.5.4 — re-assert for older forks).
		if (RunSet(con, "SET enable_http_logging = true", err)) {
			applied.push_back("enable_http_logging=true (WHY: outbound HTTP client request log; "
			                  "deprecated DuckDB setting, still effective)");
		}
	} else {
		// Force OFF — DuckDB may ship with enable_logging true; a silent skip
		// would leave QueryLog serializing every handler SQL under load.
		if (RunSet(con, "SET enable_logging = false", err)) {
			applied.push_back("enable_logging=false (WHY: default — QueryLog-per-request kills HTTP RPS; "
			                  "opt in with enable_logging:=true; use access_log for ops)");
		} else {
			applied.push_back(StringUtil::Format("enable_logging=<could not disable: %s>", err));
		}
		RunSet(con, "SET enable_http_logging = false", err);
	}

	// --- Compose quack transport log if the option exists ---
	// WHY: when duckdb-quack is loaded, share one log surface with REST. These
	// are quack's process-wide settings, so they move only under tune.
	if (opts.tune) {
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

	// --- Outbound HTTP client: curl_httpfs is mandatory ---
	// WHY: quackapi's outbound HTTP needs the pooled curl_httpfs implementation;
	// the stock DuckDB HTTPUtil is not an allowed serving client. Loading it changes
	// HTTPUtil for every consumer in this process, but selecting curl explicitly here
	// would also rewrite a DuckDB setting the caller did not ask quackapi to own.
	{
		auto curl_load = con.Query("LOAD curl_httpfs");
		if (curl_load->HasError()) {
			const auto detail = StringUtil::Replace(curl_load->GetError(), "\n", " ");
			throw InvalidInputException(
			    "quackapi_serve: required extension curl_httpfs could not be loaded: %s. "
			    "Install it with INSTALL curl_httpfs FROM community, then retry.",
			    detail);
		}
		const auto active = StringUtil::Lower(QuackapiHttpFetch::ActiveHttpUtilName(*context.db));
		if (!StringUtil::Contains(active, "curl")) {
			throw InvalidInputException(
			    "quackapi_serve: curl_httpfs loaded without activating a curl HTTPUtil; active client is '%s'",
			    QuackapiHttpFetch::ActiveHttpUtilName(*context.db));
		}
		opts.http_client_active = "curl";
		opts.http_client_reason.clear();
		applied.push_back("http_client=curl (WHY: curl_httpfs is mandatory for quackapi outbound HTTP)");
	}

	// --- Outbound timeout / retry: httpfs_timeout_retry owns the knobs ---
	// WHY: quackapi used to pin a 600s read/write ceiling of its own onto every
	// HTTPParams it built, which no operator could move. http_timeout /
	// http_retries plus this extension's per-operation overrides are the knobs,
	// and HTTPUtil::InitializeParameters already reads them.
	QuackapiRequireExtensionSetting(*context.db, "httpfs_timeout_retry", "httpfs_timeout_file_operation_ms",
	                                "quackapi_serve");
	applied.push_back("outbound_timeout=httpfs_timeout_retry (WHY: http_timeout/http_retries + per-operation "
	                  "httpfs_timeout_*_ms are the operator's knobs, not a quackapi constant)");

	// --- Observability: otlp owns the receiver ---
	// WHY: an explicitly configured quackapi_otlp is a promise the listen_url
	// implies; enforce=true turns a missing otlp into a refusal to serve rather
	// than a server that quietly records nothing.
	QuackapiOtlpReconcile(*context.db, &context, /*enforce=*/true);
	{
		auto endpoint = QuackapiState::Get(*context.db).GetOtlpEndpoint();
		applied.push_back(StringUtil::Format("otlp=%s uri=%s catalog=%s (WHY: telemetry is the otlp extension's "
		                                     "receiver; a Collector is the answer beyond local)",
		                                     endpoint.state, endpoint.uri.empty() ? "<none>" : endpoint.uri,
		                                     endpoint.catalog.empty() ? "<none>" : endpoint.catalog));
	}

	// Transport knobs are applied in QuackapiHttpServer ctor (httplib SERVER).
	applied.push_back(StringUtil::Format("http keep_alive_max_count=%d keep_alive_timeout_sec=%d "
	                                     "(WHY: connection reuse cuts TCP/TLS handshake cost)",
	                                     opts.keep_alive_max_count, opts.keep_alive_timeout_sec));
	applied.push_back(StringUtil::Format("http read_timeout_sec=%d write_timeout_sec=%d "
	                                     "(WHY: bound stalled clients so workers are not pinned forever)",
	                                     opts.read_timeout_sec, opts.write_timeout_sec));
	applied.push_back(StringUtil::Format("http worker_threads=%d (WHY: concurrent request handlers; cap prevents "
	                                     "unbounded thread spawn under load)",
	                                     opts.worker_threads));
	applied.push_back(StringUtil::Format("http payload_max_length=%llu (WHY: body size DoS guard — 413 above cap)",
	                                     (unsigned long long)QUACKAPI_PAYLOAD_MAX_LENGTH));
	applied.push_back(StringUtil::Format("access_log=%s log_level=%s (WHY: every request → structured stderr line "
	                                     "for correlation with X-Request-ID)",
	                                     opts.access_log ? "true" : "false", LogLevelDuckDBName(opts.log_level)));
	applied.push_back(StringUtil::Format("health_routes=%s (WHY: /health liveness + /healthz readiness out of the box)",
	                                     opts.health_routes ? "true" : "false"));

	string summary = StringUtil::Join(applied, "\n");
	if (opts.log_level >= QuackapiLogLevel::INFO) {
		fprintf(stderr, "quackapi: server defaults applied:\n%s\n", summary.c_str());
	}
	return summary;
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
