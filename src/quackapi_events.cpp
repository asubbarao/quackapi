#include "quackapi_events.hpp"
#include "quackapi_imports.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

namespace {

//! What a request is made of: the query, and the transaction it ran in —
//! commit AND rollback, which a span emitted from inside the process cannot
//! report once the process is the thing that died.
//!
//! connection_opened is deliberately absent: `events` fires it before DuckDB
//! assigns the connection id, so it arrives as 18446744073709551615 and
//! correlates with nothing.
const char *const DEFAULT_EVENT_TYPES[] = {"query_begin", "query_end", "transaction_begin", "transaction_commit",
                                           "transaction_rollback"};

//! The setting `events` reads per connection; quackapi writes the request id
//! into it so every later event of that request carries it.
constexpr const char *EVENTS_SESSION_NAME = "events_session_name";

string SqlLiteral(const string &value) {
	return KeywordHelper::WriteQuoted(value, '\'');
}

//! A SET lands in the caller's session, so the session is asked first and the
//! database default is only the fallback.
bool SettingValue(DatabaseInstance &db, optional_ptr<ClientContext> context, const char *name, Value &result) {
	const bool found = context ? bool(context->TryGetCurrentSetting(name, result))
	                           : bool(DBConfig::GetConfig(db).TryGetCurrentSetting(name, result));
	return found && !result.IsNull();
}

//! The configured handler command line, or empty when the sink is off.
string ConfiguredDestination(DatabaseInstance &db, optional_ptr<ClientContext> context) {
	Value value;
	if (!SettingValue(db, context, "quackapi_events", value)) {
		return string();
	}
	auto destination = value.GetValue<string>();
	StringUtil::Trim(destination);
	if (StringUtil::Lower(destination) == "off") {
		return string();
	}
	return destination;
}

vector<string> ConfiguredTypes(DatabaseInstance &db, optional_ptr<ClientContext> context) {
	vector<string> types;
	Value value;
	if (SettingValue(db, context, "quackapi_events_types", value)) {
		for (auto &child : ListValue::GetChildren(value)) {
			if (!child.IsNull()) {
				types.push_back(child.GetValue<string>());
			}
		}
	}
	if (types.empty()) {
		for (auto &name : DEFAULT_EVENT_TYPES) {
			types.push_back(name);
		}
	}
	return types;
}

Value TypesValue(const vector<string> &types) {
	vector<Value> children;
	for (auto &type : types) {
		children.push_back(Value(type));
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(children));
}

string TypesSql(const vector<string> &types) {
	string sql = "[";
	for (idx_t i = 0; i < types.size(); i++) {
		if (i > 0) {
			sql += ", ";
		}
		sql += SqlLiteral(types[i]);
	}
	return sql + "]";
}

//! Is the database-wide value already the one quackapi wants? Answered off
//! DBConfig alone — no connection, no query. Reconcile runs inside every
//! quackapi_events() call, including one inside a route, and a catalog probe
//! there would cost a connection and a query per request.
bool GlobalSettingIs(DatabaseInstance &db, const string &name, const Value &desired) {
	Value current;
	return DBConfig::GetConfig(db).TryGetCurrentSetting(name, current) && !current.IsNull() && current == desired;
}

//! One setting quackapi wants database-wide, as a value to compare and as the
//! SQL that sets it.
struct DesiredSetting {
	const char *name;
	Value value;
	string sql;
};

//! quackapi's own three are promoted alongside the events three. A request
//! connection never ran the SET that configured the sink — a plain SET is
//! session-local — so without the promotion a route asking quackapi_events()
//! what the sink is would be told 'off' while it is running, and would then
//! reset events_types to the default it computed from nothing.
vector<DesiredSetting> DesiredSettings(const QuackapiEventsSink &sink) {
	const Value destination(sink.destination);
	const Value types = TypesValue(sink.types);
	const Value async = Value::BOOLEAN(sink.async);
	const string destination_sql = SqlLiteral(sink.destination);
	const string types_sql = TypesSql(sink.types);
	const string async_sql = sink.async ? "true" : "false";
	vector<DesiredSetting> desired;
	desired.push_back({"quackapi_events", destination, destination_sql});
	desired.push_back({"quackapi_events_types", types, types_sql});
	desired.push_back({"quackapi_events_async", async, async_sql});
	// events_destination goes last: it is the switch, and the settings that
	// shape what it delivers should already be in place when it flips.
	desired.push_back({"events_types", types, types_sql});
	desired.push_back({"events_async", async, async_sql});
	desired.push_back({"events_destination", destination, destination_sql});
	return desired;
}

//! GLOBAL, not the caller's session: the connection a request is answered on
//! is not the connection that configured the sink.
bool SetGlobalSetting(DatabaseInstance &db, const string &name, const string &value_sql, string &error_out) {
	Connection con(db);
	auto res = con.Query("SET GLOBAL " + KeywordHelper::WriteOptionallyQuoted(name) + " = " + value_sql);
	if (res->HasError()) {
		error_out = StringUtil::Replace(res->GetError(), "\n", " ");
		return false;
	}
	return true;
}

//===--------------------------------------------------------------------===//
// quackapi_events()
//===--------------------------------------------------------------------===//

struct EventsBindData : public TableFunctionData {};

struct EventsGlobalState : public GlobalTableFunctionState {
	QuackapiEventsSink sink;
	bool emitted = false;
};

unique_ptr<FunctionData> EventsBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                    vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("destination");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
	names.emplace_back("types");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("async");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("state");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("detail");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("connection_id");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("transaction_id");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("session_name");
	return make_uniq<EventsBindData>();
}

unique_ptr<GlobalTableFunctionState> EventsInit(ClientContext &context, TableFunctionInitInput &) {
	auto state = make_uniq<EventsGlobalState>();
	// Re-reads the settings, so SET then SELECT is how an operator moves the
	// sink after LOAD. Inspection never fails the query: enforcement belongs
	// to serve.
	QuackapiEventsReconcile(*context.db, &context, /*enforce=*/false, state->sink);
	return std::move(state);
}

void EventsExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<EventsGlobalState>();
	if (state.emitted) {
		output.SetCardinality(0);
		return;
	}
	// The identity of THIS connection and THIS transaction: the two keys a
	// route returns so the caller can find its own events in the sink.
	const int64_t connection_id = static_cast<int64_t>(context.GetConnectionId());
	const int64_t transaction_id = context.transaction.HasActiveTransaction()
	                                   ? static_cast<int64_t>(MetaTransaction::Get(context).global_transaction_id)
	                                   : 0;
	Value session_name;
	const string stamped =
	    SettingValue(*context.db, &context, EVENTS_SESSION_NAME, session_name) ? session_name.GetValue<string>() : "";

	output.SetValue(0, 0, Value(state.sink.destination));
	output.SetValue(1, 0, TypesValue(state.sink.types));
	output.SetValue(2, 0, Value::BOOLEAN(state.sink.async));
	output.SetValue(3, 0, Value(state.sink.state));
	output.SetValue(4, 0, Value(state.sink.detail));
	output.SetValue(5, 0, Value::BIGINT(connection_id));
	output.SetValue(6, 0, Value::BIGINT(transaction_id));
	output.SetValue(7, 0, Value(stamped));
	output.SetCardinality(1);
	state.emitted = true;
}

} // namespace

//===--------------------------------------------------------------------===//
// Public API
//===--------------------------------------------------------------------===//

void QuackapiEventsReconcile(DatabaseInstance &db, optional_ptr<ClientContext> context, bool enforce,
                             QuackapiEventsSink &sink) {
	sink = QuackapiEventsSink();
	sink.destination = ConfiguredDestination(db, context);
	if (sink.destination.empty()) {
		sink.state = "off";
		sink.detail = "quackapi_events = 'off'";
		return;
	}
	sink.types = ConfiguredTypes(db, context);
	Value async_value;
	sink.async = SettingValue(db, context, "quackapi_events_async", async_value) && async_value.GetValue<bool>();
	sink.detail = sink.async ? "events (async: fire-and-forget, no delivery guarantee)" : "events";

	// Already where it should be — the ordinary case, and the one a route hits
	// on every request. Nothing below runs.
	const auto desired = DesiredSettings(sink);
	bool settled = true;
	for (auto &setting : desired) {
		if (!GlobalSettingIs(db, setting.name, setting.value)) {
			settled = false;
			break;
		}
	}
	if (settled) {
		sink.state = "serving";
		return;
	}

	// The companion gate: LOAD, never INSTALL. A download inside serve turns an
	// offline box into a server that quietly records nothing.
	try {
		QuackapiRequireExtensionSetting(db, "events", "events_destination", "quackapi_events");
	} catch (std::exception &ex) {
		sink.state = "unavailable";
		sink.detail = StringUtil::Replace(string(ex.what()), "\n", " ");
		if (enforce) {
			throw;
		}
		return;
	}

	string error;
	bool applied = true;
	for (auto &setting : desired) {
		if (!SetGlobalSetting(db, setting.name, setting.sql, error)) {
			applied = false;
			break;
		}
	}
	if (!applied) {
		sink.state = "unavailable";
		sink.detail = error;
		if (enforce) {
			throw InvalidConfigurationException("quackapi_events = '%s' could not be applied to the events "
			                                    "extension: %s",
			                                    sink.destination, error);
		}
		return;
	}
	sink.state = "serving";
}

void QuackapiEventsStampRequest(Connection &con, const string &request_id) {
	if (request_id.empty()) {
		return;
	}
	try {
		auto &context = *con.context;
		// The live sink, not quackapi's request for one: quackapi_events is
		// session-local to whoever SET it, and this connection is not that
		// session. events_destination is what reconcile made global, and it
		// is also what a caller who configured events directly would have
		// set — either way, if events are being collected, name the request.
		Value destination;
		if (!SettingValue(*context.db, &context, "events_destination", destination) ||
		    destination.GetValue<string>().empty()) {
			return;
		}
		// One statement on the request's own connection, so the stamp is
		// itself a query_begin/query_end pair in the sink. That is the price
		// of every later event of this request carrying the id the client saw
		// in X-Request-ID — including the events of a request that fails,
		// whose body says nothing at all.
		con.Query("SET " + string(EVENTS_SESSION_NAME) + " = " + SqlLiteral(request_id));
	} catch (...) {
		// The request outranks its telemetry. A sink that cannot be named is a
		// sink quackapi serves without.
	}
}

void RegisterQuackapiEventsFunctions(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	// SET quackapi_events = 'off' | '<handler program> [args…]'
	// The handler is spawned once per event with one JSON object on its stdin.
	config.AddExtensionOption("quackapi_events",
	                          "Handler program (with arguments) the events extension spawns per event, one JSON "
	                          "object per stdin. 'off' (default) leaves the events settings untouched. A "
	                          "configured handler is required at quackapi_serve and fails when events is missing.",
	                          LogicalType::VARCHAR, Value("off"));

	vector<Value> defaults;
	for (auto &name : DEFAULT_EVENT_TYPES) {
		defaults.push_back(Value(name));
	}
	// SET quackapi_events_types = ['query_begin', …]
	config.AddExtensionOption("quackapi_events_types",
	                          "Event types quackapi asks events_types for. Query and transaction lifecycle only — "
	                          "there is no row, table or CDC event in this extension.",
	                          LogicalType::LIST(LogicalType::VARCHAR), Value::LIST(LogicalType::VARCHAR, defaults));

	// SET quackapi_events_async = true — latency now, delivery never promised.
	config.AddExtensionOption("quackapi_events_async",
	                          "Fire-and-forget delivery. True keeps the handler off the query's critical path and "
	                          "gives up every delivery guarantee with it.",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));

	// Status of the sink above, plus the identity of the connection and
	// transaction the caller is on — the two keys a route returns so its
	// caller can find its own events in the handler's output.
	loader.RegisterFunction(TableFunction("quackapi_events", {}, EventsExec, EventsBind, EventsInit));
}

} // namespace duckdb
