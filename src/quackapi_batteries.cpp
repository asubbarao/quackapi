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
	// Build stamp so operators/agents can confirm the loaded extension matches the tree.
	applied.push_back(StringUtil::Format(
	    "quackapi_request_path=perf (enable_logging=%s access_log=%s) "
	    "(WHY: thread-local Connection + prepare cache + static body cache + uuidv7 ids)",
	    opts.enable_logging ? "true" : "false", opts.access_log ? "true" : "false"));
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
				throw InvalidInputException("quackapi_serve: invalid memory_limit '%s': %s", limit_to_apply, ex.what());
			}
			auto escaped = StringUtil::Replace(limit_to_apply, "'", "''");
			if (RunSet(con, StringUtil::Format("SET memory_limit TO '%s'", escaped), err)) {
				applied.push_back(
				    StringUtil::Format("memory_limit=%s (WHY: RAM guardrail for multi-query HTTP workers; "
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
			applied.push_back(StringUtil::Format("preserve_insertion_order=%s (WHY: %s)", pio,
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
			applied.push_back(
			    StringUtil::Format("enable_http_metadata_cache=%s (WHY: cache HTTP ETag/Last-Modified for "
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
			applied.push_back(StringUtil::Format("threads=%s (WHY: operator-capped worker pool for multi-tenant hosts)",
			                                     opts.threads));
		} else {
			fprintf(stderr, "quackapi: could not SET threads: %s\n", err.c_str());
		}
	} else {
		applied.push_back("threads=<DuckDB default=all cores> (WHY: max parallel query work for server)");
	}

	// --- DuckDB built-in logging (OFF by default) ---
	// WHY: QueryLog-per-handler-SQL to stdout is a multi-ms tax under load and
	// serializes workers. HTTP ops use access_log (structured stderr). Opt in
	// with enable_logging:=true when debugging query plans / errors.
	if (opts.enable_logging && opts.log_level != QuackapiLogLevel::SILENT) {
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

	// --- Outbound HTTP client: prefer curl_httpfs (pooled libcurl + HTTP/2 + async) ---
	// WHY: routes that read remote https (read_json/read_parquet/read_csv/read_text)
	// or proxy upstream APIs collapse under concurrency on DuckDB's default
	// per-request httplib client. curl_httpfs is a drop-in httpfs client layer.
	// CLIENT only — does not replace the inbound httplib SERVER.
	// Platform matrix (community description.yml excluded_platforms): unavailable on
	// wasm_* and windows_*; available on linux_* and osx_*. INSTALL/LOAD may still
	// fail (offline, old catalog) — never fail serve; fall back to httplib.
	{
		string pref = StringUtil::Lower(opts.http_client);
		StringUtil::Trim(pref);
		if (pref.empty()) {
			pref = "auto";
		}
		if (pref != "auto" && pref != "curl" && pref != "httplib") {
			throw InvalidInputException("quackapi_serve: http_client must be 'auto', 'curl', or 'httplib' (got '%s')",
			                            opts.http_client);
		}

		if (pref == "httplib") {
			// Operator forced stock client — do not INSTALL/LOAD curl_httpfs.
			// If curl_httpfs was already LOADed earlier in the process, flip the
			// backend back to httplib so this serve matches the knob.
			string set_err;
			if (RunSet(con, "SET httpfs_client_implementation = 'httplib'", set_err)) {
				// ok
			} else {
				// Setting may not exist until httpfs/curl_httpfs is loaded — fine.
				(void)set_err;
			}
			opts.http_client_active = "httplib";
			opts.http_client_reason = "operator_forced";
			fprintf(stderr, "quackapi.http_client=httplib reason=operator_forced\n");
			applied.push_back("http_client=httplib (WHY: operator forced stock httplib client; "
			                  "no curl_httpfs install)");
		} else {
			// auto | curl — prefer curl_httpfs; graceful fallback on any failure.
			string fail_detail;
			bool loaded = false;

			// Best-effort: ensure core httpfs first (curl_httpfs is 100% compatible
			// with it and usually loads it, but explicit order matches the README).
			auto httpfs_load = con.Query("LOAD httpfs");
			if (httpfs_load->HasError()) {
				auto httpfs_inst = con.Query("INSTALL httpfs");
				if (!httpfs_inst->HasError()) {
					httpfs_load = con.Query("LOAD httpfs");
				}
			}
			// httpfs failure is non-fatal here — curl_httpfs may still provide the layer.

			auto curl_load = con.Query("LOAD curl_httpfs");
			if (curl_load->HasError()) {
				auto curl_inst = con.Query("INSTALL curl_httpfs FROM community");
				if (curl_inst->HasError()) {
					fail_detail = curl_inst->GetError();
				} else {
					curl_load = con.Query("LOAD curl_httpfs");
					if (curl_load->HasError()) {
						fail_detail = curl_load->GetError();
					} else {
						loaded = true;
					}
				}
			} else {
				loaded = true;
			}

			if (loaded) {
				// Toggle the shared httpfs client backend. 'curl' selects the
				// curl-based implementation (MultiCurl / HTTPFS-Curl depending on
				// curl_httpfs version); leave multi_curl default if SET fails.
				string set_err;
				if (!RunSet(con, "SET httpfs_client_implementation = 'curl'", set_err)) {
					// Some builds expose only curl_httpfs_client_implementation.
					if (!RunSet(con, "SET curl_httpfs_client_implementation = 'curl'", set_err) &&
					    !RunSet(con, "SET curl_httpfs_client_implementation = 'multi_curl'", set_err)) {
						// Still loaded — default after LOAD is already MultiCurl.
						(void)set_err;
					}
				}
				opts.http_client_active = "curl";
				opts.http_client_reason.clear();
				fprintf(stderr, "quackapi.http_client=curl\n");
				applied.push_back("http_client=curl (WHY: curl_httpfs — libcurl pool + HTTP/2 + async IO for "
				                  "outbound https reads from handlers; 100% httpfs-compatible)");
			} else {
				opts.http_client_active = "httplib";
				opts.http_client_reason = "curl_httpfs_unavailable";
				// One structured line for ops (and a short applied summary).
				fprintf(stderr, "quackapi.http_client=httplib reason=curl_httpfs_unavailable\n");
				applied.push_back(
				    StringUtil::Format("http_client=httplib reason=curl_httpfs_unavailable (WHY: graceful fallback — "
				                       "curl_httpfs not installable/loadable on this platform or environment; "
				                       "detail=%s)",
				                       fail_detail.empty() ? "unknown" : StringUtil::Replace(fail_detail, "\n", " ")));
			}
		}
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
