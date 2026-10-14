#include "quackapi_response_encoding.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/csv_writer.hpp"
#include "duckdb/common/serializer/write_stream.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "quackapi_state.hpp"
#include "quackapi_util.hpp"

namespace duckdb {
namespace {

const string &EmptyString() {
	static const string empty;
	return empty;
}

class BoundedResponseStream : public WriteStream {
public:
	explicit BoundedResponseStream(QuackapiResponseWriter &writer) : writer(writer) {
	}

	void WriteData(const_data_ptr_t buffer, idx_t write_size) override {
		writer.Append(reinterpret_cast<const char *>(buffer), write_size);
	}

private:
	QuackapiResponseWriter &writer;
};

bool IsNestedCsvCell(const LogicalType &type) {
	auto id = type.id();
	return id == LogicalTypeId::LIST || id == LogicalTypeId::MAP || id == LogicalTypeId::STRUCT;
}

string TrimMediaToken(string value) {
	StringUtil::Trim(value);
	return value;
}

} // namespace

QuackapiResponseWriter::QuackapiResponseWriter(idx_t max_bytes)
    : max_bytes(max_bytes), bytes_written(0), exceeded_limit(false) {
}

bool QuackapiResponseWriter::Append(const char *append_data, idx_t length) {
	if (exceeded_limit) {
		return false;
	}
	if (length > max_bytes - bytes_written) {
		exceeded_limit = true;
		return false;
	}
	if (length) {
		data.append(append_data, length);
	}
	bytes_written += length;
	return true;
}

bool QuackapiResponseWriter::Append(const string &append_data) {
	return Append(append_data.data(), append_data.size());
}

bool QuackapiResponseWriter::SetMaxBytes(idx_t max_bytes) {
	this->max_bytes = max_bytes;
	if (bytes_written > max_bytes) {
		exceeded_limit = true;
	}
	return !exceeded_limit;
}

idx_t QuackapiResponseWriter::BytesWritten() const {
	return bytes_written;
}

bool QuackapiResponseWriter::ExceededLimit() const {
	return exceeded_limit;
}

const string &QuackapiResponseWriter::Data() const {
	return exceeded_limit ? EmptyString() : data;
}

class QuackapiJsonChunkEncoder::State {
public:
	State(Connection &con, const vector<string> &names, const vector<LogicalType> &types,
	      const vector<idx_t> &data_cols, JsonRowMode mode, QuackapiResponseWriter &writer)
	    : mode(mode), writer(writer), row_count(0), encoded_rows(0), finished(false) {
		if (names.size() != types.size()) {
			throw InvalidInputException("JSON encoder: result name/type count mismatch");
		}
		// json_object supplies DuckDB's own recursive serializer for every column
		// type (including MAP, STRUCT, LIST, and JSON), rather than Value::ToString.
		auto load = con.Query("LOAD json");
		if (load->HasError()) {
			throw InvalidInputException("JSON encoder: could not LOAD json: %s", load->GetError());
		}
		vector<unique_ptr<Expression>> args;
		for (auto col : data_cols) {
			if (col >= names.size()) {
				throw InvalidInputException("JSON encoder: data column index is out of bounds");
			}
			args.push_back(make_uniq<BoundConstantExpression>(Value(names[col])));
			args.push_back(make_uniq<BoundReferenceExpression>(types[col], UnsafeNumericCast<storage_t>(col)));
		}
		FunctionBinder binder(*con.context);
		ErrorData error;
		expression = binder.BindScalarFunction(DEFAULT_SCHEMA, "json_object", std::move(args), error);
		if (!expression) {
			throw InvalidInputException("JSON encoder: could not bind json_object: %s", error.Message());
		}
		executor = make_uniq<ExpressionExecutor>(*con.context, *expression);
		if (mode == JsonRowMode::ARRAY) {
			writer.Append("[");
		}
	}

	void AppendChunk(DataChunk &chunk) {
		if (finished) {
			throw InternalException("JSON encoder: cannot append after Finish");
		}
		row_count += chunk.size();
		if (writer.ExceededLimit() || chunk.size() == 0 || (mode == JsonRowMode::OBJECT && encoded_rows > 0)) {
			return;
		}
		Vector rows(LogicalType::JSON(), chunk.size());
		executor->ExecuteExpression(chunk, rows);
		UnifiedVectorFormat format;
		rows.ToUnifiedFormat(chunk.size(), format);
		auto data = format.GetData<string_t>();
		for (idx_t i = 0; i < chunk.size(); i++) {
			if (mode == JsonRowMode::ARRAY && encoded_rows > 0 && !writer.Append(",")) {
				return;
			}
			auto index = format.sel->get_index(i);
			if (!format.validity.RowIsValid(index)) {
				throw InternalException("JSON encoder: json_object returned NULL");
			}
			auto row = data[index].GetString();
			if (!writer.Append(row.data(), row.size())) {
				return;
			}
			if (mode == JsonRowMode::NDJSON && !writer.Append("\n")) {
				return;
			}
			encoded_rows++;
			if (mode == JsonRowMode::OBJECT) {
				return;
			}
		}
	}

	void Finish() {
		if (finished) {
			return;
		}
		finished = true;
		if (mode == JsonRowMode::ARRAY) {
			writer.Append("]");
		} else if (mode == JsonRowMode::OBJECT && encoded_rows == 0) {
			writer.Append("null");
		}
	}

	JsonRowMode mode;
	QuackapiResponseWriter &writer;
	idx_t row_count;
	idx_t encoded_rows;
	bool finished;
	unique_ptr<Expression> expression;
	unique_ptr<ExpressionExecutor> executor;
};

QuackapiJsonChunkEncoder::QuackapiJsonChunkEncoder(Connection &con, const vector<string> &names,
                                                   const vector<LogicalType> &types, const vector<idx_t> &data_cols,
                                                   JsonRowMode mode, QuackapiResponseWriter &writer)
    : state(make_uniq<State>(con, names, types, data_cols, mode, writer)) {
}

QuackapiJsonChunkEncoder::~QuackapiJsonChunkEncoder() = default;

QuackapiJsonChunkEncoder::QuackapiJsonChunkEncoder(QuackapiJsonChunkEncoder &&) noexcept = default;

QuackapiJsonChunkEncoder &QuackapiJsonChunkEncoder::operator=(QuackapiJsonChunkEncoder &&) noexcept = default;

void QuackapiJsonChunkEncoder::AppendChunk(DataChunk &chunk) {
	state->AppendChunk(chunk);
}

idx_t QuackapiJsonChunkEncoder::RowCount() const {
	return state->row_count;
}

void QuackapiJsonChunkEncoder::Finish() {
	state->Finish();
}

class QuackapiCsvChunkEncoder::State {
public:
	State(Connection &con, const vector<string> &names, const vector<LogicalType> &types,
	      const vector<idx_t> &data_cols, QuackapiResponseWriter &response_writer)
	    : response_writer(response_writer), stream(response_writer),
	      writer(stream, SelectNames(names, data_cols), false), writer_state(*con.context, 8192), executor(nullptr),
	      finished(false) {
		if (names.size() != types.size()) {
			throw InvalidInputException("CSV encoder: result name/type count mismatch");
		}
		bool needs_json = false;
		for (auto col : data_cols) {
			if (col >= names.size()) {
				throw InvalidInputException("CSV encoder: data column index is out of bounds");
			}
			needs_json = needs_json || IsNestedCsvCell(types[col]);
		}
		if (needs_json) {
			auto load = con.Query("LOAD json");
			if (load->HasError()) {
				throw InvalidInputException("CSV encoder: could not LOAD json: %s", load->GetError());
			}
		}
		for (auto col : data_cols) {
			unique_ptr<Expression> cell =
			    make_uniq<BoundReferenceExpression>(types[col], UnsafeNumericCast<storage_t>(col));
			if (IsNestedCsvCell(types[col])) {
				vector<unique_ptr<Expression>> args;
				args.push_back(std::move(cell));
				FunctionBinder binder(*con.context);
				ErrorData error;
				cell = binder.BindScalarFunction(DEFAULT_SCHEMA, "to_json", std::move(args), error);
				if (!cell) {
					throw InvalidInputException("CSV encoder: could not bind to_json: %s", error.Message());
				}
			}
			casts.push_back(BoundCastExpression::AddCastToType(*con.context, std::move(cell), LogicalType::VARCHAR));
			output_types.push_back(LogicalType::VARCHAR);
		}
		if (!casts.empty()) {
			executor = make_uniq<ExpressionExecutor>(*con.context, casts);
		}
		writer.options.null_str = {""};
		writer.options.dialect_options.header = CSVOption<bool>(true, true);
		writer.writer_options.newline_writing_mode = CSVNewLineMode::WRITE_AFTER;
		writer.Initialize();
		writer.WriteRawString(writer.writer_options.newline);
	}

	static vector<string> SelectNames(const vector<string> &names, const vector<idx_t> &data_cols) {
		vector<string> result;
		result.reserve(data_cols.size());
		for (auto col : data_cols) {
			if (col >= names.size()) {
				throw InvalidInputException("CSV encoder: data column index is out of bounds");
			}
			result.push_back(names[col]);
		}
		return result;
	}

	void AppendChunk(DataChunk &chunk) {
		if (finished) {
			throw InternalException("CSV encoder: cannot append after Finish");
		}
		if (chunk.size() == 0) {
			return;
		}
		if (!executor) {
			for (idx_t i = 0; i < chunk.size(); i++) {
				response_writer.Append("\n");
			}
			return;
		}
		DataChunk output;
		output.Initialize(executor->GetContext(), output_types);
		executor->Execute(chunk, output);
		writer.WriteChunk(output, writer_state);
		writer.Flush(writer_state);
	}

	void Finish() {
		if (finished) {
			return;
		}
		finished = true;
		writer.Flush(writer_state);
	}

	QuackapiResponseWriter &response_writer;
	BoundedResponseStream stream;
	CSVWriter writer;
	CSVWriterState writer_state;
	vector<unique_ptr<Expression>> casts;
	vector<LogicalType> output_types;
	unique_ptr<ExpressionExecutor> executor;
	bool finished;
};

QuackapiCsvChunkEncoder::QuackapiCsvChunkEncoder(Connection &con, const vector<string> &names,
                                                 const vector<LogicalType> &types, const vector<idx_t> &data_cols,
                                                 QuackapiResponseWriter &writer)
    : state(make_uniq<State>(con, names, types, data_cols, writer)) {
}

QuackapiCsvChunkEncoder::~QuackapiCsvChunkEncoder() = default;

QuackapiCsvChunkEncoder::QuackapiCsvChunkEncoder(QuackapiCsvChunkEncoder &&) noexcept = default;

QuackapiCsvChunkEncoder &QuackapiCsvChunkEncoder::operator=(QuackapiCsvChunkEncoder &&) noexcept = default;

void QuackapiCsvChunkEncoder::AppendChunk(DataChunk &chunk) {
	state->AppendChunk(chunk);
}

void QuackapiCsvChunkEncoder::Finish() {
	state->Finish();
}

//! Render a DuckDB Value as JSON. The database typed the column — the JSON
//! representation follows the type, not string-formatting heuristics.
string ValueToJson(const Value &value) {
	if (value.IsNull()) {
		return "null";
	}
	auto &type = value.type();
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return value.GetValue<bool>() ? "true" : "false";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::DECIMAL:
		return value.ToString();
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE: {
		auto d = value.GetValue<double>();
		if (std::isnan(d) || std::isinf(d)) {
			// JSON has no NaN/Infinity — mirror FastAPI/ujson and emit null
			return "null";
		}
		return value.ToString();
	}
	case LogicalTypeId::LIST: {
		auto &children = ListValue::GetChildren(value);
		string result = "[";
		for (idx_t i = 0; i < children.size(); i++) {
			if (i > 0) {
				result += ",";
			}
			result += ValueToJson(children[i]);
		}
		result += "]";
		return result;
	}
	case LogicalTypeId::STRUCT: {
		auto &children = StructValue::GetChildren(value);
		auto &child_types = StructType::GetChildTypes(type);
		string result = "{";
		for (idx_t i = 0; i < children.size(); i++) {
			if (i > 0) {
				result += ",";
			}
			result += "\"" + QuackapiJsonEscape(child_types[i].first) + "\":" + ValueToJson(children[i]);
		}
		result += "}";
		return result;
	}
	default:
		return "\"" + QuackapiJsonEscape(value.ToString()) + "\"";
	}
}

//! True if column name is a special response control column (stripped from body).
bool IsSpecialResponseColumn(const string &name) {
	auto lower = StringUtil::Lower(name);
	return lower == "location" || lower == "set_cookie" || lower == "set-cookie";
}

void SetJson(duckdb_httplib::Response &res, int status, const string &body) {
	res.status = status;
	res.set_content(body, "application/json");
}

//! Response mode inferred from the single output column's name — the database
//! names the payload. `html` -> text/html, `text` -> text/plain; anything else
//! serializes as row data (JSON / NDJSON / CSV via FORMAT + Accept).

ResponseMode ResponseModeFor(const vector<string> &names) {
	if (names.size() != 1) {
		return ResponseMode::JSON;
	}
	auto lower = StringUtil::Lower(names[0]);
	if (lower == "html") {
		return ResponseMode::HTML;
	}
	if (lower == "text") {
		return ResponseMode::TEXT;
	}
	return ResponseMode::JSON;
}

//! Wire format for multi-row data responses (not html/text column modes).

BodyFormat ParseBodyFormatToken(const string &token) {
	auto t = StringUtil::Lower(token);
	if (t == "ndjson") {
		return BodyFormat::NDJSON;
	}
	if (t == "csv") {
		return BodyFormat::CSV;
	}
	return BodyFormat::JSON;
}

//! Map Accept media-type (no parameters) to BodyFormat. Returns false if unknown.
bool MediaTypeToBodyFormat(const string &media, BodyFormat &out) {
	auto m = StringUtil::Lower(media);
	// strip parameters already; still trim
	m = TrimMediaToken(m);
	if (m == "application/x-ndjson" || m == "application/jsonl" || m == "application/ndjson") {
		out = BodyFormat::NDJSON;
		return true;
	}
	if (m == "text/csv" || m == "application/csv") {
		out = BodyFormat::CSV;
		return true;
	}
	if (m == "application/json" || m == "text/json" || m == "*/*") {
		out = BodyFormat::JSON;
		return true;
	}
	return false;
}

//! Parse one Accept token's q-value (default 1.0).
double ParseAcceptQ(const string &token) {
	auto parts = StringUtil::Split(token, ';');
	if (parts.size() < 2) {
		return 1.0;
	}
	for (idx_t i = 1; i < parts.size(); i++) {
		auto p = TrimMediaToken(parts[i]);
		auto pl = StringUtil::Lower(p);
		if (StringUtil::StartsWith(pl, "q=")) {
			return atof(p.c_str() + 2);
		}
	}
	return 1.0;
}

//! When route FORMAT is default/json, honor Accept for ndjson/csv/json.
//! Explicit route FORMAT (ndjson|csv) always wins.
BodyFormat ResolveBodyFormat(const QuackapiRoute &route, const duckdb_httplib::Request &req) {
	auto route_fmt = StringUtil::Lower(route.response_format.empty() ? "json" : route.response_format);
	if (route_fmt == "ndjson" || route_fmt == "csv") {
		return ParseBodyFormatToken(route_fmt);
	}
	// Default / explicit json: negotiate Accept when present.
	auto it = req.headers.find("Accept");
	if (it == req.headers.end() || it->second.empty()) {
		return BodyFormat::JSON;
	}
	// Pick the highest-q recognized type among json / ndjson / csv.
	BodyFormat best = BodyFormat::JSON;
	double best_q = -1.0;
	bool saw_recognized = false;
	auto tokens = StringUtil::Split(it->second, ',');
	for (auto &tok : tokens) {
		auto t = TrimMediaToken(tok);
		if (t.empty()) {
			continue;
		}
		double q = ParseAcceptQ(t);
		if (q <= 0.0) {
			continue;
		}
		auto semi = t.find(';');
		string media = semi == string::npos ? t : TrimMediaToken(t.substr(0, semi));
		BodyFormat f;
		if (!MediaTypeToBodyFormat(media, f)) {
			continue;
		}
		// Prefer a concrete non-json format when q ties with JSON.
		bool better = !saw_recognized || q > best_q;
		if (!better && q == best_q) {
			// Prefer non-json when client listed it at same q after json — first highest wins
			// unless current best is from */* and this is concrete.
			if (best == BodyFormat::JSON && f != BodyFormat::JSON) {
				better = true;
			}
		}
		if (better) {
			best = f;
			best_q = q;
			saw_recognized = true;
		}
	}
	return saw_recognized ? best : BodyFormat::JSON;
}

string EmptyResultBody(const QuackapiRoute &route) {
	if (!route.empty_body.empty()) {
		return route.empty_body;
	}
	return "{\"detail\":\"Not Found\"}";
}

void SetInternalError(duckdb_httplib::Response &res, const string &server_side_detail) {
	// Return DuckDB's diagnostic so clients can diagnose route execution failures.
	fprintf(stderr, "quackapi: internal error: %s\n", server_side_detail.c_str());
	SetJson(res, 500, "{\"detail\":\"" + QuackapiJsonEscape(server_side_detail) + "\"}");
}

} // namespace duckdb
