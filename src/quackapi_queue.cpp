#include "quackapi_queue.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parser_extension.hpp"

#include "quackapi_state.hpp"

#include <cmath>
#include "quackapi_util.hpp"

namespace duckdb {

namespace {

//===--------------------------------------------------------------------===//
// SQL helpers
//===--------------------------------------------------------------------===//

//! Escape a string for use as a single-quoted SQL literal.
string SqlQuote(const string &s) {
	string out = "'";
	for (char c : s) {
		if (c == '\'') {
			out += "''";
		} else {
			out += c;
		}
	}
	out += "'";
	return out;
}

void CheckQuery(unique_ptr<MaterializedQueryResult> &res, const string &ctx) {
	if (res->HasError()) {
		throw InvalidInputException("quackapi queue %s: %s", ctx, res->GetError());
	}
}

void RunSQL(DatabaseInstance &db, const string &sql, const string &ctx) {
	Connection con(db);
	auto res = con.Query(sql);
	CheckQuery(res, ctx);
}

} // namespace

void EnsureQuackapiJobsTable(DatabaseInstance &db) {
	// One statement per Query — DuckDB does not accept multi-statement strings here.
	RunSQL(db, "CREATE SEQUENCE IF NOT EXISTS quackapi_job_seq START 1", "create sequence");
	// payload is VARCHAR holding JSON text — avoids a hard runtime dep on LOAD
	// json while still accepting payload::JSON at the function boundary (cast to
	// text on insert). Users can CAST(payload AS JSON) when the json extension
	// is loaded.
	RunSQL(db,
	       "CREATE TABLE IF NOT EXISTS quackapi_jobs ("
	       "id BIGINT PRIMARY KEY, "
	       "queue VARCHAR NOT NULL, "
	       "payload VARCHAR NOT NULL, "
	       "status VARCHAR NOT NULL DEFAULT 'pending', "
	       "attempts INTEGER NOT NULL DEFAULT 0, "
	       "max_attempts INTEGER NOT NULL DEFAULT 3, "
	       "visible_at TIMESTAMP NOT NULL, "
	       "created_at TIMESTAMP NOT NULL, "
	       "updated_at TIMESTAMP NOT NULL, "
	       "last_error VARCHAR, "
	       "worker_id VARCHAR"
	       ")",
	       "create table");
	RunSQL(db,
	       "CREATE INDEX IF NOT EXISTS idx_quackapi_jobs_ready "
	       "ON quackapi_jobs (queue, status, visible_at, id)",
	       "create index");
}

namespace {

//===--------------------------------------------------------------------===//
// Duration parse: 30 | '30' | '30s' | '5m' | '1h'
//===--------------------------------------------------------------------===//

bool ParseDurationSeconds(const string &raw, int32_t &out_sec, string &err) {
	auto s = QuackapiTrim(raw);
	if (s.empty()) {
		err = "empty duration";
		return false;
	}
	// Strip surrounding quotes if present.
	if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
		s = s.substr(1, s.size() - 2);
	}
	if (s.empty()) {
		err = "empty duration";
		return false;
	}
	idx_t i = 0;
	bool neg = false;
	if (s[i] == '-') {
		neg = true;
		i++;
	}
	if (i >= s.size() || !StringUtil::CharacterIsDigit(s[i])) {
		err = "duration must start with a number";
		return false;
	}
	int64_t num = 0;
	while (i < s.size() && StringUtil::CharacterIsDigit(s[i])) {
		num = num * 10 + (s[i] - '0');
		if (num > 86400LL * 365) {
			err = "duration too large";
			return false;
		}
		i++;
	}
	int64_t mult = 1;
	if (i < s.size()) {
		auto unit = StringUtil::Lower(s.substr(i));
		if (unit == "s" || unit == "sec" || unit == "secs" || unit == "second" || unit == "seconds") {
			mult = 1;
		} else if (unit == "m" || unit == "min" || unit == "mins" || unit == "minute" || unit == "minutes") {
			mult = 60;
		} else if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" || unit == "hours") {
			mult = 3600;
		} else {
			err = "unknown duration unit \"" + unit + "\" — use s, m, or h";
			return false;
		}
	}
	int64_t sec = num * mult;
	if (neg) {
		sec = -sec;
	}
	if (sec < 0) {
		err = "duration must be non-negative";
		return false;
	}
	if (sec > 86400LL * 7) {
		err = "visibility_timeout max is 7 days";
		return false;
	}
	out_sec = static_cast<int32_t>(sec);
	return true;
}

//===--------------------------------------------------------------------===//
// CREATE / DROP QUEUE parser
//===--------------------------------------------------------------------===//

struct QueueDdlParseData : public ParserExtensionParseData {
	string action; // CREATE / DROP
	bool or_replace = false;
	QuackapiQueue queue;

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto copy = make_uniq<QueueDdlParseData>();
		copy->action = action;
		copy->or_replace = or_replace;
		copy->queue = queue;
		return std::move(copy);
	}
	string ToString() const override {
		return action + " QUEUE " + queue.name;
	}
};

//! Grammar:
//!   CREATE [OR REPLACE] QUEUE <name>
//!     [WITH ( max_attempts=<n> , visibility_timeout='30s'|30 , backoff_base_seconds=<n> )]
//!   DROP QUEUE <name>
ParserExtensionParseResult QueueDdlParse(ParserExtensionInfo *, const string &query) {
	auto q = QuackapiTrim(query);
	auto upper = StringUtil::Upper(q);

	bool or_replace = false;
	idx_t pos;
	if (StringUtil::StartsWith(upper, "CREATE QUEUE ")) {
		pos = 13;
	} else if (StringUtil::StartsWith(upper, "CREATE OR REPLACE QUEUE ")) {
		pos = 24;
		or_replace = true;
	} else if (StringUtil::StartsWith(upper, "DROP QUEUE ")) {
		auto name = QuackapiTrim(q.substr(11));
		if (name.empty() || name.find(' ') != string::npos) {
			return ParserExtensionParseResult("DROP QUEUE expects a single queue name");
		}
		auto data = make_uniq<QueueDdlParseData>();
		data->action = "DROP";
		data->queue.name = name;
		return ParserExtensionParseResult(std::move(data));
	} else {
		return ParserExtensionParseResult();
	}

	auto rest = QuackapiTrim(q.substr(pos));
	if (rest.empty()) {
		return ParserExtensionParseResult("CREATE QUEUE expects a queue name");
	}

	// <name> — bare identifier up to space or end or WITH
	idx_t name_end = 0;
	while (name_end < rest.size() && !StringUtil::CharacterIsSpace(rest[name_end]) && rest[name_end] != '(') {
		name_end++;
	}
	auto name = rest.substr(0, name_end);
	if (name.empty()) {
		return ParserExtensionParseResult("CREATE QUEUE expects a queue name");
	}
	// Reject path-like names
	for (char c : name) {
		if (!(StringUtil::CharacterIsAlpha(c) || StringUtil::CharacterIsDigit(c) || c == '_' || c == '-')) {
			return ParserExtensionParseResult("Queue name must be an identifier ([A-Za-z0-9_-]+)");
		}
	}
	rest = QuackapiTrim(rest.substr(name_end));
	auto rest_upper = StringUtil::Upper(rest);

	QuackapiQueue queue;
	queue.name = name;
	// defaults already set on QuackapiQueue

	// optional WITH ( ... )
	if (!rest.empty()) {
		if (!(StringUtil::StartsWith(rest_upper, "WITH") &&
		      (rest.size() == 4 || StringUtil::CharacterIsSpace(rest[4]) || rest[4] == '('))) {
			return ParserExtensionParseResult("Expected WITH (...) after queue name, or end of statement");
		}
		rest = QuackapiTrim(rest.substr(4));
		if (rest.empty() || rest[0] != '(') {
			return ParserExtensionParseResult("WITH expects a parenthesized option list");
		}
		auto close = rest.find(')');
		if (close == string::npos) {
			return ParserExtensionParseResult("Unterminated WITH ( ... ) options");
		}
		auto opts = QuackapiTrim(rest.substr(1, close - 1));
		rest = QuackapiTrim(rest.substr(close + 1));
		if (!rest.empty()) {
			return ParserExtensionParseResult("Unexpected tokens after WITH options");
		}

		// Parse comma-separated key=value pairs
		idx_t oi = 0;
		while (oi < opts.size()) {
			while (oi < opts.size() && (StringUtil::CharacterIsSpace(opts[oi]) || opts[oi] == ',')) {
				oi++;
			}
			if (oi >= opts.size()) {
				break;
			}
			// key
			idx_t key_start = oi;
			while (oi < opts.size() && (StringUtil::CharacterIsAlpha(opts[oi]) ||
			                            StringUtil::CharacterIsDigit(opts[oi]) || opts[oi] == '_')) {
				oi++;
			}
			auto key = StringUtil::Lower(opts.substr(key_start, oi - key_start));
			while (oi < opts.size() && StringUtil::CharacterIsSpace(opts[oi])) {
				oi++;
			}
			if (oi >= opts.size() || opts[oi] != '=') {
				return ParserExtensionParseResult("WITH options: expected key=value (got key \"" + key + "\")");
			}
			oi++; // =
			while (oi < opts.size() && StringUtil::CharacterIsSpace(opts[oi])) {
				oi++;
			}
			if (oi >= opts.size()) {
				return ParserExtensionParseResult("WITH options: missing value for \"" + key + "\"");
			}
			// value: quoted string or bare token up to comma
			string val;
			if (opts[oi] == '\'') {
				idx_t j = oi + 1;
				string raw;
				while (j < opts.size()) {
					if (opts[j] == '\'') {
						if (j + 1 < opts.size() && opts[j + 1] == '\'') {
							raw += '\'';
							j += 2;
							continue;
						}
						break;
					}
					raw += opts[j];
					j++;
				}
				if (j >= opts.size() || opts[j] != '\'') {
					return ParserExtensionParseResult("Unterminated quoted value for \"" + key + "\"");
				}
				val = raw;
				oi = j + 1;
			} else {
				idx_t j = oi;
				while (j < opts.size() && opts[j] != ',' && !StringUtil::CharacterIsSpace(opts[j])) {
					j++;
				}
				val = opts.substr(oi, j - oi);
				oi = j;
			}

			if (key == "max_attempts") {
				int v = atoi(val.c_str());
				if (v < 1 || v > 1000) {
					return ParserExtensionParseResult("max_attempts must be between 1 and 1000");
				}
				queue.max_attempts = v;
			} else if (key == "visibility_timeout" || key == "visibility_timeout_sec") {
				int32_t sec = 0;
				string err;
				if (!ParseDurationSeconds(val, sec, err)) {
					return ParserExtensionParseResult("visibility_timeout: " + err);
				}
				if (sec < 1) {
					return ParserExtensionParseResult("visibility_timeout must be at least 1 second");
				}
				queue.visibility_timeout_sec = sec;
			} else if (key == "backoff_base_seconds" || key == "backoff_base") {
				int v = atoi(val.c_str());
				if (v < 0 || v > 3600) {
					return ParserExtensionParseResult("backoff_base_seconds must be between 0 and 3600");
				}
				queue.backoff_base_sec = v;
			} else {
				return ParserExtensionParseResult(
				    "Unknown QUEUE option \"" + key +
				    "\" — expected max_attempts, visibility_timeout, backoff_base_seconds");
			}
		}
	}

	auto data = make_uniq<QueueDdlParseData>();
	data->action = "CREATE";
	data->or_replace = or_replace;
	data->queue = queue;
	return ParserExtensionParseResult(std::move(data));
}

struct ApplyQueueBindData : public TableFunctionData {
	string action;
	bool or_replace = false;
	string name;
	int32_t max_attempts = 3;
	int32_t visibility_timeout_sec = 30;
	int32_t backoff_base_sec = 2;
	bool finished = false;
};

unique_ptr<FunctionData> ApplyQueueBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ApplyQueueBindData>();
	bind_data->action = input.inputs[0].GetValue<string>();
	bind_data->or_replace = input.inputs[1].GetValue<bool>();
	bind_data->name = input.inputs[2].GetValue<string>();
	bind_data->max_attempts = input.inputs[3].GetValue<int32_t>();
	bind_data->visibility_timeout_sec = input.inputs[4].GetValue<int32_t>();
	bind_data->backoff_base_sec = input.inputs[5].GetValue<int32_t>();
	BindStatusColumn(return_types, names);
	return std::move(bind_data);
}

void ApplyQueueExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ApplyQueueBindData>();
	if (bind_data.finished) {
		return;
	}
	auto &state = QuackapiState::Get(*context.db);
	string message;
	if (bind_data.action == "CREATE") {
		// Durable jobs table lives in the user's catalog / .db file.
		EnsureQuackapiJobsTable(*context.db);
		QuackapiQueue q;
		q.name = bind_data.name;
		q.max_attempts = bind_data.max_attempts;
		q.visibility_timeout_sec = bind_data.visibility_timeout_sec;
		q.backoff_base_sec = bind_data.backoff_base_sec;
		state.AddQueue(q, bind_data.or_replace);
		message = StringUtil::Format("Queue %s: max_attempts=%d visibility_timeout=%ds", q.name, q.max_attempts,
		                             q.visibility_timeout_sec);
	} else {
		if (state.DropQueue(bind_data.name)) {
			message = StringUtil::Format("Dropped queue %s", bind_data.name);
		} else {
			throw InvalidInputException("Queue \"%s\" does not exist", bind_data.name);
		}
	}
	EmitOneShotStatus(output, bind_data.finished, message);
}

TableFunction MakeApplyQueueFunction() {
	return MakeApplyDdlFunction("quackapi_apply_queue",
	                            {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::INTEGER,
	                             LogicalType::INTEGER, LogicalType::INTEGER},
	                            ApplyQueueExec, ApplyQueueBind);
}

ParserExtensionPlanResult QueueDdlPlan(ParserExtensionInfo *, ClientContext &,
                                       unique_ptr<ParserExtensionParseData> parse_data) {
	auto &data = static_cast<QueueDdlParseData &>(*parse_data);
	ParserExtensionPlanResult result;
	result.function = MakeApplyQueueFunction();
	result.parameters.push_back(Value(data.action));
	result.parameters.push_back(Value::BOOLEAN(data.or_replace));
	result.parameters.push_back(Value(data.queue.name));
	result.parameters.push_back(Value::INTEGER(data.queue.max_attempts));
	result.parameters.push_back(Value::INTEGER(data.queue.visibility_timeout_sec));
	result.parameters.push_back(Value::INTEGER(data.queue.backoff_base_sec));
	FinishDdlPlan(result);
	return result;
}

//===--------------------------------------------------------------------===//
// Lookup helper
//===--------------------------------------------------------------------===//

QuackapiQueue RequireQueue(DatabaseInstance &db, const string &name) {
	QuackapiQueue q;
	if (!QuackapiState::Get(db).GetQueue(name, q)) {
		throw InvalidInputException("Queue \"%s\" does not exist — CREATE QUEUE first", name);
	}
	return q;
}

//===--------------------------------------------------------------------===//
// quackapi_enqueue(queue, payload [, max_attempts]) → job_id BIGINT
//===--------------------------------------------------------------------===//

int64_t EnqueueJob(DatabaseInstance &db, const string &queue_name, const string &payload_json,
                   int32_t max_attempts_override) {
	auto q = RequireQueue(db, queue_name);
	EnsureQuackapiJobsTable(db);
	int32_t max_att = max_attempts_override > 0 ? max_attempts_override : q.max_attempts;
	if (max_att < 1) {
		throw InvalidInputException("quackapi_enqueue: max_attempts must be >= 1");
	}
	// Store payload as JSON text (VARCHAR). Callers may pass JSON or VARCHAR.
	string sql =
	    StringUtil::Format("INSERT INTO quackapi_jobs "
	                       "(id, queue, payload, status, attempts, max_attempts, visible_at, created_at, updated_at) "
	                       "SELECT nextval('quackapi_job_seq'), %s, %s, 'pending', 0, %d, "
	                       "now()::TIMESTAMP, now()::TIMESTAMP, now()::TIMESTAMP "
	                       "RETURNING id",
	                       SqlQuote(queue_name), SqlQuote(payload_json), max_att);
	Connection con(db);
	auto res = con.Query(sql);
	CheckQuery(res, "enqueue");
	if (res->RowCount() == 0) {
		throw InternalException("quackapi_enqueue: INSERT RETURNING produced no row");
	}
	return res->GetValue(0, 0).GetValue<int64_t>();
}

//! enqueue(queue, payload) / enqueue(queue, payload, max_attempts) — one body.
//! Optional 3rd arg via args.ColumnCount() (same pattern as nack).
void EnqueueScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &db = *state.GetContext().db;
	UnifiedVectorFormat qdata, pdata, mdata;
	args.data[0].ToUnifiedFormat(args.size(), qdata);
	args.data[1].ToUnifiedFormat(args.size(), pdata);
	const bool has_max = args.ColumnCount() >= 3;
	if (has_max) {
		args.data[2].ToUnifiedFormat(args.size(), mdata);
	}
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetData<int64_t>(result);
	auto &validity = FlatVector::Validity(result);
	for (idx_t i = 0; i < args.size(); i++) {
		auto qi = qdata.sel->get_index(i);
		auto pi = pdata.sel->get_index(i);
		if (!qdata.validity.RowIsValid(qi) || !pdata.validity.RowIsValid(pi)) {
			validity.SetInvalid(i);
			continue;
		}
		auto qn = UnifiedVectorFormat::GetData<string_t>(qdata)[qi].GetString();
		auto payload = UnifiedVectorFormat::GetData<string_t>(pdata)[pi].GetString();
		int32_t max_att = 0;
		if (has_max) {
			auto mi = mdata.sel->get_index(i);
			if (mdata.validity.RowIsValid(mi)) {
				max_att = UnifiedVectorFormat::GetData<int32_t>(mdata)[mi];
			}
		}
		out[i] = EnqueueJob(db, qn, payload, max_att);
	}
}

//===--------------------------------------------------------------------===//
// quackapi_dequeue(queue [, n]) table function — claim jobs
//===--------------------------------------------------------------------===//

struct DequeueBindData : public TableFunctionData {
	string queue_name;
	int32_t n = 1;
};

struct DequeueGlobalState : public GlobalTableFunctionState {
	vector<vector<Value>> rows; // pre-claimed rows
	idx_t offset = 0;
};

unique_ptr<FunctionData> DequeueBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                     vector<string> &names) {
	auto bind_data = make_uniq<DequeueBindData>();
	bind_data->queue_name = input.inputs[0].GetValue<string>();
	if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
		bind_data->n = input.inputs[1].GetValue<int32_t>();
	}
	if (bind_data->n < 1) {
		throw InvalidInputException("quackapi_dequeue: n must be >= 1");
	}
	if (bind_data->n > 1000) {
		throw InvalidInputException("quackapi_dequeue: n max is 1000");
	}
	return_types = {LogicalType::BIGINT,  LogicalType::VARCHAR, LogicalType::VARCHAR,   LogicalType::VARCHAR,
	                LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::TIMESTAMP, LogicalType::VARCHAR};
	names = {"id", "queue", "payload", "status", "attempts", "max_attempts", "visible_at", "last_error"};
	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> DequeueInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<DequeueBindData>();
	auto state = make_uniq<DequeueGlobalState>();
	auto q = RequireQueue(*context.db, bind.queue_name);
	EnsureQuackapiJobsTable(*context.db);

	// Atomic claim under DuckDB single-writer: one UPDATE…RETURNING per job.
	// Ready set: pending OR running-with-expired-lease (visibility timeout).
	Connection con(*context.db);
	for (int32_t i = 0; i < bind.n; i++) {
		string sql =
		    StringUtil::Format("UPDATE quackapi_jobs SET "
		                       "status = 'running', "
		                       "attempts = attempts + 1, "
		                       "visible_at = now()::TIMESTAMP + to_seconds(%d), "
		                       "updated_at = now()::TIMESTAMP, "
		                       "worker_id = 'dequeue' "
		                       "WHERE id = ("
		                       "  SELECT id FROM quackapi_jobs "
		                       "  WHERE queue = %s "
		                       "    AND status IN ('pending', 'running') "
		                       "    AND visible_at <= now()::TIMESTAMP "
		                       "  ORDER BY id LIMIT 1"
		                       ") "
		                       "RETURNING id, queue, payload, status, attempts, max_attempts, visible_at, last_error",
		                       q.visibility_timeout_sec, SqlQuote(bind.queue_name));
		auto res = con.Query(sql);
		CheckQuery(res, "dequeue");
		if (res->RowCount() == 0) {
			break;
		}
		vector<Value> row;
		for (idx_t c = 0; c < 8; c++) {
			row.push_back(res->GetValue(c, 0));
		}
		state->rows.push_back(std::move(row));
	}
	return std::move(state);
}

void DequeueExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DequeueGlobalState>();
	idx_t row = 0;
	while (state.offset < state.rows.size() && row < STANDARD_VECTOR_SIZE) {
		auto &r = state.rows[state.offset];
		for (idx_t c = 0; c < 8; c++) {
			output.SetValue(c, row, r[c]);
		}
		row++;
		state.offset++;
	}
	output.SetCardinality(row);
}

//===--------------------------------------------------------------------===//
// quackapi_ack(queue, job_id) → BOOLEAN
//===--------------------------------------------------------------------===//

bool AckJob(DatabaseInstance &db, const string &queue_name, int64_t job_id) {
	RequireQueue(db, queue_name);
	EnsureQuackapiJobsTable(db);
	string sql =
	    StringUtil::Format("UPDATE quackapi_jobs SET status = 'done', updated_at = now()::TIMESTAMP, worker_id = NULL "
	                       "WHERE id = %lld AND queue = %s AND status = 'running' "
	                       "RETURNING id",
	                       static_cast<long long>(job_id), SqlQuote(queue_name));
	Connection con(db);
	auto res = con.Query(sql);
	CheckQuery(res, "ack");
	return res->RowCount() > 0;
}

void AckScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &db = *state.GetContext().db;
	UnifiedVectorFormat qdata, idata;
	args.data[0].ToUnifiedFormat(args.size(), qdata);
	args.data[1].ToUnifiedFormat(args.size(), idata);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetData<bool>(result);
	auto &validity = FlatVector::Validity(result);
	for (idx_t i = 0; i < args.size(); i++) {
		auto qi = qdata.sel->get_index(i);
		auto ii = idata.sel->get_index(i);
		if (!qdata.validity.RowIsValid(qi) || !idata.validity.RowIsValid(ii)) {
			validity.SetInvalid(i);
			continue;
		}
		auto qn = UnifiedVectorFormat::GetData<string_t>(qdata)[qi].GetString();
		auto jid = UnifiedVectorFormat::GetData<int64_t>(idata)[ii];
		out[i] = AckJob(db, qn, jid);
	}
}

//===--------------------------------------------------------------------===//
// quackapi_nack(queue, job_id [, requeue [, error]]) → VARCHAR status
//===--------------------------------------------------------------------===//

string NackJob(DatabaseInstance &db, const string &queue_name, int64_t job_id, bool requeue, const string &error) {
	auto q = RequireQueue(db, queue_name);
	EnsureQuackapiJobsTable(db);

	// Read current attempts/max for decision; single-writer so this is safe.
	string read_sql = StringUtil::Format("SELECT attempts, max_attempts FROM quackapi_jobs "
	                                     "WHERE id = %lld AND queue = %s AND status = 'running'",
	                                     static_cast<long long>(job_id), SqlQuote(queue_name));
	Connection con(db);
	auto read = con.Query(read_sql);
	CheckQuery(read, "nack read");
	if (read->RowCount() == 0) {
		throw InvalidInputException("quackapi_nack: job %lld not running on queue \"%s\"",
		                            static_cast<long long>(job_id), queue_name);
	}
	int32_t attempts = read->GetValue(0, 0).GetValue<int32_t>();
	int32_t max_att = read->GetValue(1, 0).GetValue<int32_t>();

	bool to_dead = !requeue || attempts >= max_att;
	int32_t backoff = 0;
	if (!to_dead) {
		// exponential: base^min(attempts, 6). backoff_base_sec=0 means immediate retry.
		if (q.backoff_base_sec > 0) {
			int exp = attempts;
			if (exp > 6) {
				exp = 6;
			}
			if (exp < 0) {
				exp = 0;
			}
			double secs = std::pow(static_cast<double>(q.backoff_base_sec), exp);
			if (secs > 3600.0) {
				secs = 3600.0;
			}
			backoff = static_cast<int32_t>(secs);
		}
	}

	string sql;
	if (to_dead) {
		sql = StringUtil::Format("UPDATE quackapi_jobs SET status = 'dead', "
		                         "visible_at = now()::TIMESTAMP, "
		                         "last_error = %s, updated_at = now()::TIMESTAMP, worker_id = NULL "
		                         "WHERE id = %lld AND queue = %s AND status = 'running' "
		                         "RETURNING status",
		                         SqlQuote(error.empty() ? "nack" : error), static_cast<long long>(job_id),
		                         SqlQuote(queue_name));
	} else {
		sql = StringUtil::Format("UPDATE quackapi_jobs SET status = 'pending', "
		                         "visible_at = now()::TIMESTAMP + to_seconds(%d), "
		                         "last_error = %s, updated_at = now()::TIMESTAMP, worker_id = NULL "
		                         "WHERE id = %lld AND queue = %s AND status = 'running' "
		                         "RETURNING status",
		                         backoff, SqlQuote(error.empty() ? "nack" : error), static_cast<long long>(job_id),
		                         SqlQuote(queue_name));
	}
	auto res = con.Query(sql);
	CheckQuery(res, "nack");
	if (res->RowCount() == 0) {
		throw InvalidInputException("quackapi_nack: job %lld disappeared", static_cast<long long>(job_id));
	}
	return res->GetValue(0, 0).GetValue<string>();
}

//! nack(queue, job_id [, requeue [, error]]) — one body, optional args by arity.
void NackScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &db = *state.GetContext().db;
	UnifiedVectorFormat qdata, idata, rdata, edata;
	args.data[0].ToUnifiedFormat(args.size(), qdata);
	args.data[1].ToUnifiedFormat(args.size(), idata);
	const bool has_requeue = args.ColumnCount() >= 3;
	const bool has_error = args.ColumnCount() >= 4;
	if (has_requeue) {
		args.data[2].ToUnifiedFormat(args.size(), rdata);
	}
	if (has_error) {
		args.data[3].ToUnifiedFormat(args.size(), edata);
	}
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetData<string_t>(result);
	auto &validity = FlatVector::Validity(result);
	for (idx_t i = 0; i < args.size(); i++) {
		auto qi = qdata.sel->get_index(i);
		auto ii = idata.sel->get_index(i);
		if (!qdata.validity.RowIsValid(qi) || !idata.validity.RowIsValid(ii)) {
			validity.SetInvalid(i);
			continue;
		}
		auto qn = UnifiedVectorFormat::GetData<string_t>(qdata)[qi].GetString();
		auto jid = UnifiedVectorFormat::GetData<int64_t>(idata)[ii];
		bool requeue = true;
		if (has_requeue) {
			auto ri = rdata.sel->get_index(i);
			if (rdata.validity.RowIsValid(ri)) {
				requeue = UnifiedVectorFormat::GetData<bool>(rdata)[ri];
			}
		}
		string err = "nack";
		if (has_error) {
			auto ei = edata.sel->get_index(i);
			if (edata.validity.RowIsValid(ei)) {
				err = UnifiedVectorFormat::GetData<string_t>(edata)[ei].GetString();
			}
		}
		auto st = NackJob(db, qn, jid, requeue, err);
		out[i] = StringVector::AddString(result, st);
	}
}

//===--------------------------------------------------------------------===//
// quackapi_queues() — name, depth, in_flight, dead (+ options)
//===--------------------------------------------------------------------===//

struct QueuesBindData : public TableFunctionData {};

struct QueuesGlobalState : public GlobalTableFunctionState {
	vector<QuackapiQueue> queues;
	// Parallel stats vectors indexed same as queues
	vector<int64_t> depth;
	vector<int64_t> in_flight;
	vector<int64_t> dead;
	idx_t offset = 0;
};

unique_ptr<FunctionData> QueuesBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                    vector<string> &names) {
	return_types = {LogicalType::VARCHAR, LogicalType::BIGINT,  LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER};
	names = {"name", "depth", "in_flight", "dead", "max_attempts", "visibility_timeout_sec", "backoff_base_sec"};
	return make_uniq<QueuesBindData>();
}

unique_ptr<GlobalTableFunctionState> QueuesInit(ClientContext &context, TableFunctionInitInput &) {
	auto state = make_uniq<QueuesGlobalState>();
	state->queues = QuackapiState::Get(*context.db).SnapshotQueues();
	state->depth.assign(state->queues.size(), 0);
	state->in_flight.assign(state->queues.size(), 0);
	state->dead.assign(state->queues.size(), 0);

	// Jobs table may not exist yet if no CREATE QUEUE ran (empty registry).
	if (state->queues.empty()) {
		return std::move(state);
	}
	try {
		EnsureQuackapiJobsTable(*context.db);
	} catch (...) {
		return std::move(state);
	}
	Connection con(*context.db);
	// depth = claimable (pending OR running with expired lease)
	// in_flight = running with active lease
	// dead = dead
	auto res =
	    con.Query("SELECT queue, "
	              "count(*) FILTER (WHERE status = 'pending' OR "
	              "  (status = 'running' AND visible_at <= now()::TIMESTAMP))::BIGINT AS depth, "
	              "count(*) FILTER (WHERE status = 'running' AND visible_at > now()::TIMESTAMP)::BIGINT AS in_flight, "
	              "count(*) FILTER (WHERE status = 'dead')::BIGINT AS dead "
	              "FROM quackapi_jobs GROUP BY queue");
	if (res->HasError()) {
		return std::move(state);
	}
	unordered_map<string, idx_t> idx;
	for (idx_t i = 0; i < state->queues.size(); i++) {
		idx[state->queues[i].name] = i;
	}
	for (idx_t r = 0; r < res->RowCount(); r++) {
		auto qn = res->GetValue(0, r).GetValue<string>();
		auto it = idx.find(qn);
		if (it == idx.end()) {
			continue; // orphaned jobs from dropped queue — ignore for this view
		}
		state->depth[it->second] = res->GetValue(1, r).GetValue<int64_t>();
		state->in_flight[it->second] = res->GetValue(2, r).GetValue<int64_t>();
		state->dead[it->second] = res->GetValue(3, r).GetValue<int64_t>();
	}
	return std::move(state);
}

void QueuesExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<QueuesGlobalState>();
	idx_t row = 0;
	while (state.offset < state.queues.size() && row < STANDARD_VECTOR_SIZE) {
		auto &q = state.queues[state.offset];
		output.SetValue(0, row, Value(q.name));
		output.SetValue(1, row, Value::BIGINT(state.depth[state.offset]));
		output.SetValue(2, row, Value::BIGINT(state.in_flight[state.offset]));
		output.SetValue(3, row, Value::BIGINT(state.dead[state.offset]));
		output.SetValue(4, row, Value::INTEGER(q.max_attempts));
		output.SetValue(5, row, Value::INTEGER(q.visibility_timeout_sec));
		output.SetValue(6, row, Value::INTEGER(q.backoff_base_sec));
		row++;
		state.offset++;
	}
	output.SetCardinality(row);
}

} // namespace

//===--------------------------------------------------------------------===//
// Public registration
//===--------------------------------------------------------------------===//

QueueDdlParserExtension::QueueDdlParserExtension() {
	parse_function = QueueDdlParse;
	plan_function = QueueDdlPlan;
}

TableFunction GetApplyQueueFunction() {
	return MakeApplyQueueFunction();
}

void RegisterQuackapiQueueFunctions(ExtensionLoader &loader) {
	// enqueue(queue, payload) / enqueue(queue, payload, max_attempts)
	ScalarFunctionSet enqueue_set("quackapi_enqueue");
	enqueue_set.AddFunction(ScalarFunction("quackapi_enqueue", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                       LogicalType::BIGINT, EnqueueScalar));
	enqueue_set.AddFunction(ScalarFunction("quackapi_enqueue",
	                                       {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER},
	                                       LogicalType::BIGINT, EnqueueScalar));
	// JSON payload overload (JSON is a logical type alias of VARCHAR storage)
	enqueue_set.AddFunction(ScalarFunction("quackapi_enqueue", {LogicalType::VARCHAR, LogicalType::JSON()},
	                                       LogicalType::BIGINT, EnqueueScalar));
	enqueue_set.AddFunction(ScalarFunction("quackapi_enqueue",
	                                       {LogicalType::VARCHAR, LogicalType::JSON(), LogicalType::INTEGER},
	                                       LogicalType::BIGINT, EnqueueScalar));
	loader.RegisterFunction(enqueue_set);

	// dequeue(queue) / dequeue(queue, n)
	TableFunctionSet dequeue_set("quackapi_dequeue");
	TableFunction dequeue1("quackapi_dequeue", {LogicalType::VARCHAR}, DequeueExec, DequeueBind, DequeueInit);
	TableFunction dequeue2("quackapi_dequeue", {LogicalType::VARCHAR, LogicalType::INTEGER}, DequeueExec, DequeueBind,
	                       DequeueInit);
	dequeue_set.AddFunction(dequeue1);
	dequeue_set.AddFunction(dequeue2);
	loader.RegisterFunction(dequeue_set);

	// ack(queue, job_id) → bool
	loader.RegisterFunction(
	    ScalarFunction("quackapi_ack", {LogicalType::VARCHAR, LogicalType::BIGINT}, LogicalType::BOOLEAN, AckScalar));

	// nack(queue, job_id) / nack(queue, job_id, requeue) / nack(queue, job_id, requeue, error)
	ScalarFunctionSet nack_set("quackapi_nack");
	nack_set.AddFunction(
	    ScalarFunction("quackapi_nack", {LogicalType::VARCHAR, LogicalType::BIGINT}, LogicalType::VARCHAR, NackScalar));
	nack_set.AddFunction(ScalarFunction("quackapi_nack",
	                                    {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BOOLEAN},
	                                    LogicalType::VARCHAR, NackScalar));
	nack_set.AddFunction(ScalarFunction(
	    "quackapi_nack", {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BOOLEAN, LogicalType::VARCHAR},
	    LogicalType::VARCHAR, NackScalar));
	loader.RegisterFunction(nack_set);

	// queues() inspection
	loader.RegisterFunction(TableFunction("quackapi_queues", {}, QueuesExec, QueuesBind, QueuesInit));
}

} // namespace duckdb
