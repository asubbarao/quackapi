#include "quackapi_stream.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/parser/parser_extension.hpp"

#include "quackapi_state.hpp"
#include "quackapi_util.hpp"

namespace duckdb {

namespace {

//! Parse duration to milliseconds: 30 | '30' | '30s' | '500ms' | '1m' | '1h'
bool ParseIntervalMs(const string &raw, int64_t &out_ms, string &err) {
	auto s = QuackapiTrim(raw);
	if (s.empty()) {
		err = "empty interval";
		return false;
	}
	if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
		s = s.substr(1, s.size() - 2);
	}
	if (s.empty()) {
		err = "empty interval";
		return false;
	}
	idx_t i = 0;
	if (s[i] == '-') {
		err = "interval must be non-negative";
		return false;
	}
	if (i >= s.size() || !StringUtil::CharacterIsDigit(s[i])) {
		err = "interval must start with a number";
		return false;
	}
	int64_t num = 0;
	while (i < s.size() && StringUtil::CharacterIsDigit(s[i])) {
		num = num * 10 + (s[i] - '0');
		if (num > 86400LL * 1000) {
			err = "interval too large";
			return false;
		}
		i++;
	}
	int64_t mult_ms = 1000; // bare number = seconds (same as queue-style durations)
	if (i < s.size()) {
		auto unit = StringUtil::Lower(s.substr(i));
		if (unit == "ms" || unit == "msec" || unit == "millis" || unit == "millisecond" || unit == "milliseconds") {
			mult_ms = 1;
		} else if (unit == "s" || unit == "sec" || unit == "secs" || unit == "second" || unit == "seconds") {
			mult_ms = 1000;
		} else if (unit == "m" || unit == "min" || unit == "mins" || unit == "minute" || unit == "minutes") {
			mult_ms = 60 * 1000;
		} else if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" || unit == "hours") {
			mult_ms = 3600 * 1000;
		} else {
			err = "unknown interval unit \"" + unit + "\" — use ms, s, m, or h";
			return false;
		}
	}
	int64_t ms = num * mult_ms;
	if (ms > 86400LL * 1000) {
		err = "interval max is 24 hours";
		return false;
	}
	out_ms = ms;
	return true;
}

//===--------------------------------------------------------------------===//
// CREATE / DROP STREAM parser
//===--------------------------------------------------------------------===//

struct StreamDdlParseData : public ParserExtensionParseData {
	string action; // CREATE / DROP
	bool or_replace = false;
	QuackapiStream stream;

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto copy = make_uniq<StreamDdlParseData>();
		copy->action = action;
		copy->or_replace = or_replace;
		copy->stream = stream;
		return std::move(copy);
	}
	string ToString() const override {
		return action + " STREAM " + stream.name;
	}
};

//! Grammar:
//!   CREATE [OR REPLACE] STREAM <name> GET '<path>'
//!     [WITH (interval='1s'|1000)]
//!     AS <select>
//!   DROP STREAM <name>
ParserExtensionParseResult StreamDdlParse(ParserExtensionInfo *, const string &query) {
	auto q = QuackapiTrim(query);
	auto upper = StringUtil::Upper(q);

	bool or_replace = false;
	idx_t pos;
	if (StringUtil::StartsWith(upper, "CREATE STREAM ")) {
		pos = 14;
	} else if (StringUtil::StartsWith(upper, "CREATE OR REPLACE STREAM ")) {
		pos = 25;
		or_replace = true;
	} else if (StringUtil::StartsWith(upper, "DROP STREAM ")) {
		auto name = QuackapiTrim(q.substr(12));
		if (name.empty() || name.find(' ') != string::npos) {
			return ParserExtensionParseResult("DROP STREAM expects a single stream name");
		}
		auto data = make_uniq<StreamDdlParseData>();
		data->action = "DROP";
		data->stream.name = name;
		return ParserExtensionParseResult(std::move(data));
	} else {
		return ParserExtensionParseResult();
	}

	auto rest = QuackapiTrim(q.substr(pos));
	// <name> <METHOD>
	auto first_space = rest.find(' ');
	if (first_space == string::npos) {
		return ParserExtensionParseResult("CREATE STREAM <name> GET '<path>' AS <select>");
	}
	auto name = rest.substr(0, first_space);
	for (char c : name) {
		if (!(StringUtil::CharacterIsAlpha(c) || StringUtil::CharacterIsDigit(c) || c == '_' || c == '-')) {
			return ParserExtensionParseResult("Stream name must be an identifier ([A-Za-z0-9_-]+)");
		}
	}
	rest = QuackapiTrim(rest.substr(first_space));
	auto second_space = rest.find(' ');
	if (second_space == string::npos) {
		return ParserExtensionParseResult("Expected GET '<path>' after stream name");
	}
	auto method = StringUtil::Upper(rest.substr(0, second_space));
	if (method == "WS" || method == "WEBSOCKET" || method == "WSS") {
		return ParserExtensionParseResult(
		    "CREATE STREAM WebSocket is not supported: bundled cpp-httplib has no WebSocket/Upgrade API. "
		    "Use CREATE STREAM <name> GET '<path>' for Server-Sent Events (text/event-stream). "
		    "Bidirectional duplex belongs on the quack protocol, not HTTP Upgrade.");
	}
	if (method != "GET") {
		return ParserExtensionParseResult("CREATE STREAM only supports GET (SSE). Unknown method \"" + method +
		                                  "\" — WebSocket is deferred (no WS on httplib transport)");
	}
	rest = QuackapiTrim(rest.substr(second_space));

	// '<path>'
	if (rest.empty() || rest[0] != '\'') {
		return ParserExtensionParseResult("Expected quoted '<path>' after GET");
	}
	auto path_end = rest.find('\'', 1);
	if (path_end == string::npos) {
		return ParserExtensionParseResult("Unterminated stream path");
	}
	auto pattern = rest.substr(1, path_end - 1);
	if (pattern.empty() || pattern[0] != '/') {
		return ParserExtensionParseResult("Stream path must start with '/'");
	}
	rest = QuackapiTrim(rest.substr(path_end + 1));
	auto rest_upper = StringUtil::Upper(rest);

	QuackapiStream stream;
	stream.name = name;
	stream.method = "GET";
	stream.pattern = pattern;
	stream.interval_ms = 0;
	stream.transport = QuackapiStreamTransport::SSE;

	// optional WITH ( interval=... )
	if (StringUtil::StartsWith(rest_upper, "WITH") &&
	    (rest.size() == 4 || StringUtil::CharacterIsSpace(rest[4]) || rest[4] == '(')) {
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
		rest_upper = StringUtil::Upper(rest);

		idx_t oi = 0;
		while (oi < opts.size()) {
			while (oi < opts.size() && (StringUtil::CharacterIsSpace(opts[oi]) || opts[oi] == ',')) {
				oi++;
			}
			if (oi >= opts.size()) {
				break;
			}
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
			oi++;
			while (oi < opts.size() && StringUtil::CharacterIsSpace(opts[oi])) {
				oi++;
			}
			if (oi >= opts.size()) {
				return ParserExtensionParseResult("WITH options: missing value for \"" + key + "\"");
			}
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

			if (key == "interval") {
				int64_t ms = 0;
				string err;
				if (!ParseIntervalMs(val, ms, err)) {
					return ParserExtensionParseResult("interval: " + err);
				}
				stream.interval_ms = ms;
			} else {
				return ParserExtensionParseResult("Unknown STREAM option \"" + key + "\" — expected interval");
			}
		}
	}

	// AS <select>
	if (!(StringUtil::StartsWith(rest_upper, "AS") && rest.size() > 2 && StringUtil::CharacterIsSpace(rest[2]))) {
		return ParserExtensionParseResult("Expected AS <select> in CREATE STREAM");
	}
	auto handler = QuackapiTrim(rest.substr(2));
	if (handler.empty()) {
		return ParserExtensionParseResult("Empty handler after AS");
	}
	stream.handler_sql = handler;

	auto data = make_uniq<StreamDdlParseData>();
	data->action = "CREATE";
	data->or_replace = or_replace;
	data->stream = stream;
	return ParserExtensionParseResult(std::move(data));
}

struct ApplyStreamBindData : public TableFunctionData {
	string action;
	bool or_replace = false;
	QuackapiStream stream;
	bool finished = false;
};

unique_ptr<FunctionData> ApplyStreamBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ApplyStreamBindData>();
	bind_data->action = input.inputs[0].GetValue<string>();
	bind_data->or_replace = input.inputs[1].GetValue<bool>();
	bind_data->stream.name = input.inputs[2].GetValue<string>();
	bind_data->stream.method = input.inputs[3].GetValue<string>();
	bind_data->stream.pattern = input.inputs[4].GetValue<string>();
	bind_data->stream.handler_sql = input.inputs[5].GetValue<string>();
	bind_data->stream.interval_ms = input.inputs[6].GetValue<int64_t>();
	bind_data->stream.transport = QuackapiStreamTransport::SSE;
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("status");
	return std::move(bind_data);
}

void ApplyStreamExec(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->CastNoConst<ApplyStreamBindData>();
	if (bind_data.finished) {
		return;
	}
	auto &state = QuackapiState::Get(*context.db);
	string message;
	if (bind_data.action == "CREATE") {
		{
			Connection con(*context.db);
			auto prepared = con.Prepare(bind_data.stream.handler_sql);
			if (prepared->HasError()) {
				throw InvalidInputException("Invalid handler SQL for stream \"%s\": %s", bind_data.stream.name,
				                            prepared->GetError());
			}
		}
		state.AddStream(bind_data.stream, bind_data.or_replace);
		if (bind_data.stream.interval_ms > 0) {
			message = StringUtil::Format("Stream %s: GET %s SSE interval=%lldms", bind_data.stream.name,
			                             bind_data.stream.pattern, (long long)bind_data.stream.interval_ms);
		} else {
			message = StringUtil::Format("Stream %s: GET %s SSE", bind_data.stream.name, bind_data.stream.pattern);
		}
	} else {
		if (state.DropStream(bind_data.stream.name)) {
			message = StringUtil::Format("Dropped stream %s", bind_data.stream.name);
		} else {
			throw InvalidInputException("Stream \"%s\" does not exist", bind_data.stream.name);
		}
	}
	output.SetValue(0, 0, Value(message));
	output.SetCardinality(1);
	bind_data.finished = true;
}

TableFunction MakeApplyStreamFunction() {
	TableFunction function("quackapi_apply_stream",
	                       {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                        LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT},
	                       ApplyStreamExec, ApplyStreamBind);
	return function;
}

ParserExtensionPlanResult StreamDdlPlan(ParserExtensionInfo *, ClientContext &,
                                        unique_ptr<ParserExtensionParseData> parse_data) {
	auto &data = static_cast<StreamDdlParseData &>(*parse_data);
	ParserExtensionPlanResult result;
	result.function = MakeApplyStreamFunction();
	result.parameters.push_back(Value(data.action));
	result.parameters.push_back(Value::BOOLEAN(data.or_replace));
	result.parameters.push_back(Value(data.stream.name));
	result.parameters.push_back(Value(data.stream.method));
	result.parameters.push_back(Value(data.stream.pattern));
	result.parameters.push_back(Value(data.stream.handler_sql));
	result.parameters.push_back(Value::BIGINT(data.stream.interval_ms));
	result.requires_valid_transaction = false;
	result.return_type = StatementReturnType::QUERY_RESULT;
	return result;
}

//===--------------------------------------------------------------------===//
// quackapi_streams()
//===--------------------------------------------------------------------===//

struct StreamsBindData : public TableFunctionData {};

struct StreamsGlobalState : public GlobalTableFunctionState {
	vector<QuackapiStream> streams;
	idx_t offset = 0;
};

unique_ptr<FunctionData> StreamsBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                     vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("method");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("pattern");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("transport");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("interval_ms");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("handler");
	return make_uniq<StreamsBindData>();
}

unique_ptr<GlobalTableFunctionState> StreamsInit(ClientContext &context, TableFunctionInitInput &) {
	auto state = make_uniq<StreamsGlobalState>();
	state->streams = QuackapiState::Get(*context.db).SnapshotStreams();
	return std::move(state);
}

void StreamsExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<StreamsGlobalState>();
	idx_t row = 0;
	while (state.offset < state.streams.size() && row < STANDARD_VECTOR_SIZE) {
		auto &s = state.streams[state.offset];
		output.SetValue(0, row, Value(s.name));
		output.SetValue(1, row, Value(s.method));
		output.SetValue(2, row, Value(s.pattern));
		output.SetValue(3, row, Value("sse"));
		output.SetValue(4, row, Value::BIGINT(s.interval_ms));
		output.SetValue(5, row, Value(s.handler_sql));
		row++;
		state.offset++;
	}
	output.SetCardinality(row);
}

} // namespace

StreamDdlParserExtension::StreamDdlParserExtension() {
	parse_function = StreamDdlParse;
	plan_function = StreamDdlPlan;
}

TableFunction GetApplyStreamFunction() {
	return MakeApplyStreamFunction();
}

void RegisterQuackapiStreamFunctions(ExtensionLoader &loader) {
	loader.RegisterFunction(TableFunction("quackapi_streams", {}, StreamsExec, StreamsBind, StreamsInit));
}

} // namespace duckdb
