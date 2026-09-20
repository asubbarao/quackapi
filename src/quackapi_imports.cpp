#include "quackapi_imports.hpp"
#include "quackapi_server.hpp"
#include "quackapi_state.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

namespace {

//! The endpoint quackapi default-creates in local mode. Loopback on purpose:
//! an OTLP receiver reachable from off-box is a Collector's job, not a
//! database's.
constexpr const char *OTLP_LOCAL_URI = "otlp:localhost:4318";

//! Every second, matching the drain recipe the queue guide has always shown.
constexpr const char *QUEUE_WORKER_SCHEDULE = "*/1 * * * * *";

string SqlLiteral(const string &value) {
	return KeywordHelper::WriteQuoted(value, '\'');
}

//! Non-lossy probe: keep the matching names, read the number off the list.
bool CatalogRowPresent(DatabaseInstance &db, const string &relation, const string &column, const string &value) {
	Connection con(db);
	auto res = con.Query("SELECT array_agg(DISTINCT " + column + ") AS names, len(names) AS n FROM " + relation +
	                     " WHERE " + column + " = " + SqlLiteral(value));
	if (res->HasError() || res->RowCount() == 0) {
		return false;
	}
	auto n = res->GetValue(1, 0);
	return !n.IsNull() && n.GetValue<int64_t>() > 0;
}

bool FunctionPresent(DatabaseInstance &db, const string &function_name) {
	return CatalogRowPresent(db, "duckdb_functions()", "function_name", function_name);
}

bool SettingPresent(DatabaseInstance &db, const string &setting_name) {
	return CatalogRowPresent(db, "duckdb_settings()", "name", setting_name);
}

//! Is the companion usable right now? A name already in the catalog answers
//! without a LOAD; otherwise LOAD it — never INSTALL — and let that answer.
//! The catalog is not re-probed afterwards: during extension autoload the
//! system catalog has not published the new entries yet, and a probe there
//! reports a companion that LOADed cleanly as missing.
bool CompanionReady(DatabaseInstance &db, const string &extension, const string &probe, bool probe_is_setting,
                    string &error_out) {
	error_out.clear();
	if (probe_is_setting ? SettingPresent(db, probe) : FunctionPresent(db, probe)) {
		return true;
	}
	Connection con(db);
	auto load = con.Query("LOAD " + KeywordHelper::WriteOptionallyQuoted(extension));
	if (!load->HasError()) {
		return true;
	}
	error_out = StringUtil::Replace(load->GetError(), "\n", " ");
	return false;
}

//! A SET lands in the caller's session, so the session is asked first and the
//! database default is only the fallback.
string SettingText(DatabaseInstance &db, optional_ptr<ClientContext> context, const char *name,
                   const string &fallback) {
	Value value;
	bool found = context ? context->TryGetCurrentSetting(name, value)
	                     : DBConfig::GetConfig(db).TryGetCurrentSetting(name, value);
	if (!found || value.IsNull()) {
		return fallback;
	}
	auto text = value.GetValue<string>();
	StringUtil::Trim(text);
	return text.empty() ? fallback : text;
}

QuackapiLogLevel ConfiguredLogLevel(DatabaseInstance &db, optional_ptr<ClientContext> context) {
	try {
		return ParseQuackapiLogLevel(SettingText(db, context, "quackapi_log_level", "info"));
	} catch (...) {
		return QuackapiLogLevel::INFO;
	}
}

void AnnounceOtlp(DatabaseInstance &db, optional_ptr<ClientContext> context, const QuackapiOtlpEndpoint &endpoint) {
	if (ConfiguredLogLevel(db, context) < QuackapiLogLevel::INFO) {
		return;
	}
	if (endpoint.state == "off") {
		return;
	}
	if (endpoint.state == "serving") {
		fprintf(stderr,
		        "quackapi: OTLP endpoint %s is serving inside this process, for this box only. "
		        "Anything beyond local wants an OpenTelemetry Collector in front of it — quackapi "
		        "collects nothing of its own. Durable ingest: SET quackapi_otlp_catalog to a DuckLake "
		        "or Iceberg catalog (currently %s).\n",
		        endpoint.uri.c_str(), endpoint.catalog.empty() ? "unset" : endpoint.catalog.c_str());
		return;
	}
	fprintf(stderr,
	        "quackapi: no OTLP endpoint — %s. quackapi records no requests of its own; observability "
	        "is the otlp extension's, and beyond local an OpenTelemetry Collector's.\n",
	        endpoint.detail.c_str());
}

//===--------------------------------------------------------------------===//
// quackapi_otlp()
//===--------------------------------------------------------------------===//

struct OtlpBindData : public TableFunctionData {};

struct OtlpGlobalState : public GlobalTableFunctionState {
	QuackapiOtlpEndpoint endpoint;
	bool emitted = false;
};

unique_ptr<FunctionData> OtlpBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                  vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("uri");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("catalog");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("state");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("detail");
	return make_uniq<OtlpBindData>();
}

unique_ptr<GlobalTableFunctionState> OtlpInit(ClientContext &context, TableFunctionInitInput &) {
	auto state = make_uniq<OtlpGlobalState>();
	// Re-reads the settings, so a SET after LOAD is visible here rather than
	// stale. Inspection never fails the query: enforcement belongs to serve.
	QuackapiOtlpReconcile(*context.db, &context, /*enforce=*/false);
	state->endpoint = QuackapiState::Get(*context.db).GetOtlpEndpoint();
	return std::move(state);
}

void OtlpExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<OtlpGlobalState>();
	if (state.emitted) {
		output.SetCardinality(0);
		return;
	}
	output.SetValue(0, 0, Value(state.endpoint.uri));
	output.SetValue(1, 0, Value(state.endpoint.catalog));
	output.SetValue(2, 0, Value(state.endpoint.state));
	output.SetValue(3, 0, Value(state.endpoint.detail));
	output.SetCardinality(1);
	state.emitted = true;
}

//===--------------------------------------------------------------------===//
// quackapi_queue_worker()
//===--------------------------------------------------------------------===//

struct QueueWorkerBindData : public TableFunctionData {
	string queue;
	string schedule;
	string sql;
	string job_id;
};

struct QueueWorkerGlobalState : public GlobalTableFunctionState {
	bool emitted = false;
};

unique_ptr<FunctionData> QueueWorkerBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto bind = make_uniq<QueueWorkerBindData>();
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw InvalidInputException("quackapi_queue_worker(queue): queue must be non-NULL");
	}
	bind->queue = input.inputs[0].GetValue<string>();

	auto &db = *context.db;
	QuackapiQueue queue;
	if (!QuackapiState::Get(db).GetQueue(bind->queue, queue)) {
		throw InvalidInputException("quackapi_queue_worker: queue '%s' is not registered; CREATE QUEUE it first",
		                            bind->queue);
	}

	bind->schedule = QUEUE_WORKER_SCHEDULE;
	int64_t batch = 1;
	for (auto &named : input.named_parameters) {
		if (named.second.IsNull()) {
			continue;
		}
		if (named.first == "schedule") {
			bind->schedule = named.second.GetValue<string>();
		} else if (named.first == "sql") {
			bind->sql = named.second.GetValue<string>();
		} else if (named.first == "batch") {
			batch = named.second.GetValue<int64_t>();
		}
	}
	if (batch < 1) {
		throw InvalidInputException("quackapi_queue_worker: batch must be >= 1");
	}
	if (bind->sql.empty()) {
		bind->sql = "SELECT quackapi_ack(" + SqlLiteral(bind->queue) +
		            ", id, delivery_generation) AS acked FROM quackapi_dequeue(" + SqlLiteral(bind->queue) + ", " +
		            std::to_string(batch) + ")";
	}

	// The runner is cronjob's. quackapi owns the queue table and the drain SQL;
	// keeping a thread pool of its own here would be the same scheduler written
	// twice, and worse the second time.
	QuackapiRequireExtension(db, "cronjob", "cron", "quackapi_queue_worker");

	Connection con(db);
	auto res = con.Query("SELECT cron(" + SqlLiteral(bind->sql) + ", " + SqlLiteral(bind->schedule) + ") AS job_id");
	if (res->HasError()) {
		throw InvalidInputException("quackapi_queue_worker: cron() rejected the schedule '%s': %s", bind->schedule,
		                            StringUtil::Replace(res->GetError(), "\n", " "));
	}
	auto job = res->GetValue(0, 0);
	bind->job_id = job.IsNull() ? string() : job.GetValue<string>();

	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("queue");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("job_id");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("schedule");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("sql");
	return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> QueueWorkerInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<QueueWorkerGlobalState>();
}

void QueueWorkerExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<QueueWorkerGlobalState>();
	if (state.emitted) {
		output.SetCardinality(0);
		return;
	}
	auto &bind = data_p.bind_data->Cast<QueueWorkerBindData>();
	output.SetValue(0, 0, Value(bind.queue));
	output.SetValue(1, 0, Value(bind.job_id));
	output.SetValue(2, 0, Value(bind.schedule));
	output.SetValue(3, 0, Value(bind.sql));
	output.SetCardinality(1);
	state.emitted = true;
}

} // namespace

//===--------------------------------------------------------------------===//
// Public API
//===--------------------------------------------------------------------===//

void QuackapiRequireExtension(DatabaseInstance &db, const string &extension, const string &probe_function,
                              const string &feature) {
	string load_error;
	if (CompanionReady(db, extension, probe_function, /*probe_is_setting=*/false, load_error)) {
		return;
	}
	throw InvalidConfigurationException("%s requires the '%s' extension: %s. Install it with INSTALL %s FROM "
	                                    "community, then retry.",
	                                    feature, extension, load_error, extension);
}

void QuackapiRequireExtensionSetting(DatabaseInstance &db, const string &extension, const string &probe_setting,
                                     const string &feature) {
	string load_error;
	if (CompanionReady(db, extension, probe_setting, /*probe_is_setting=*/true, load_error)) {
		return;
	}
	throw InvalidConfigurationException("%s requires the '%s' extension: %s. Install it with INSTALL %s FROM "
	                                    "community, then retry.",
	                                    feature, extension, load_error, extension);
}

void QuackapiOtlpReconcile(DatabaseInstance &db, optional_ptr<ClientContext> context, bool enforce) {
	auto &state = QuackapiState::Get(db);
	const auto configured = SettingText(db, context, "quackapi_otlp", "local");
	const auto mode = StringUtil::Lower(configured);

	QuackapiOtlpEndpoint endpoint;
	endpoint.catalog = SettingText(db, context, "quackapi_otlp_catalog", string());

	if (mode == "off") {
		endpoint.state = "off";
		endpoint.detail = "quackapi_otlp = 'off'";
		state.SetOtlpEndpoint(endpoint);
		return;
	}
	const bool explicit_uri = mode != "local";
	endpoint.uri = explicit_uri ? configured : string(OTLP_LOCAL_URI);

	string load_error;
	if (!CompanionReady(db, "otlp", "otlp_serve", /*probe_is_setting=*/false, load_error)) {
		endpoint.state = "unavailable";
		endpoint.detail = StringUtil::Format("the 'otlp' extension is not loaded (%s)", load_error);
		state.SetOtlpEndpoint(endpoint);
		if (explicit_uri && enforce) {
			throw InvalidConfigurationException(
			    "quackapi_otlp = '%s' requires the 'otlp' extension: %s. Install it with INSTALL otlp FROM "
			    "community, then retry.",
			    configured, endpoint.detail);
		}
		AnnounceOtlp(db, context, endpoint);
		return;
	}

	auto previous = state.GetOtlpEndpoint();
	if (previous.state == "serving" && previous.uri == endpoint.uri && previous.catalog == endpoint.catalog) {
		return;
	}

	// otlp_serve owns the receiver, the OTLP schema and the DuckLake/Iceberg
	// write path. quackapi only decides that a local one exists by default.
	string sql = "SELECT * FROM otlp_serve(" + SqlLiteral(endpoint.uri) + ", create_tables := true";
	if (!endpoint.catalog.empty()) {
		sql += ", catalog := " + SqlLiteral(endpoint.catalog);
	}
	sql += ")";
	Connection con(db);
	auto res = con.Query(sql);
	if (res->HasError()) {
		endpoint.state = "error";
		endpoint.detail = StringUtil::Replace(res->GetError(), "\n", " ");
		state.SetOtlpEndpoint(endpoint);
		if (explicit_uri && enforce) {
			throw InvalidConfigurationException("quackapi_otlp = '%s' could not be served: %s", configured,
			                                    endpoint.detail);
		}
		AnnounceOtlp(db, context, endpoint);
		return;
	}
	endpoint.state = "serving";
	endpoint.detail = "otlp_serve";
	state.SetOtlpEndpoint(endpoint);
	AnnounceOtlp(db, context, endpoint);
}

void RegisterQuackapiImportFunctions(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	// SET quackapi_otlp = 'local' | 'off' | 'otlp:host:port'
	// 'local' default-creates the loopback receiver on LOAD when the otlp
	// extension is loaded; an explicit URI makes otlp mandatory at serve.
	config.AddExtensionOption("quackapi_otlp",
	                          "OTLP/HTTP endpoint quackapi creates through the otlp extension: "
	                          "'local' (default, " +
	                              string(OTLP_LOCAL_URI) +
	                              "), 'off', or an explicit otlp: URI. An explicit URI is "
	                              "required at quackapi_serve and fails when otlp is missing.",
	                          LogicalType::VARCHAR, Value("local"));
	// SET quackapi_otlp_catalog = 'my_ducklake' — durable ingest target.
	config.AddExtensionOption("quackapi_otlp_catalog",
	                          "DuckLake or Iceberg catalog otlp_serve writes spans/metrics/logs into. "
	                          "Empty (default) keeps them in this database's tables.",
	                          LogicalType::VARCHAR, Value(""));

	// Status of the endpoint above. Also re-reads the settings, so SET then
	// SELECT is how an operator moves it after LOAD.
	loader.RegisterFunction(TableFunction("quackapi_otlp", {}, OtlpExec, OtlpBind, OtlpInit));

	// Queue drain with a runner: cronjob executes the SELECT on a schedule.
	// A server already obliged to stay online gives cron the uptime an external
	// scheduler exists to work around.
	TableFunction worker("quackapi_queue_worker", {LogicalType::VARCHAR}, QueueWorkerExec, QueueWorkerBind,
	                     QueueWorkerInit);
	worker.named_parameters["schedule"] = LogicalType::VARCHAR;
	worker.named_parameters["sql"] = LogicalType::VARCHAR;
	worker.named_parameters["batch"] = LogicalType::BIGINT;
	loader.RegisterFunction(worker);
}

} // namespace duckdb
