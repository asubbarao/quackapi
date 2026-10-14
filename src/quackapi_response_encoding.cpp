#include "quackapi_response_encoding.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "quackapi_state.hpp"
#include "quackapi_util.hpp"

namespace duckdb {
namespace {

string TrimMediaToken(string value) {
	StringUtil::Trim(value);
	return value;
}

} // namespace

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
//! serializes as row data (json / ndjson / csv / parquet / arrow via FORMAT + Accept).

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
	if (t == "parquet") {
		return BodyFormat::PARQUET;
	}
	if (t == "arrow" || t == "arrows") {
		return BodyFormat::ARROW;
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
	if (m == "application/vnd.apache.parquet" || m == "application/parquet") {
		out = BodyFormat::PARQUET;
		return true;
	}
	// Arrow IPC stream (what nanoarrow FORMAT ARROWS emits) or file.
	if (m == "application/vnd.apache.arrow.stream" || m == "application/vnd.apache.arrow.file" ||
	    m == "application/x-apache-arrow-stream" || m == "application/arrow") {
		out = BodyFormat::ARROW;
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

//! When route FORMAT is default/json, honor Accept for ndjson/csv/parquet/arrow/json.
//! Explicit route FORMAT (ndjson|csv|parquet|arrow) always wins.
BodyFormat ResolveBodyFormat(const QuackapiRoute &route, const duckdb_httplib::Request &req) {
	auto route_fmt = StringUtil::Lower(route.response_format.empty() ? "json" : route.response_format);
	if (route_fmt == "ndjson" || route_fmt == "csv" || route_fmt == "parquet" || route_fmt == "arrow" ||
	    route_fmt == "arrows") {
		return ParseBodyFormatToken(route_fmt);
	}
	// Default / explicit json: negotiate Accept when present.
	auto it = req.headers.find("Accept");
	if (it == req.headers.end() || it->second.empty()) {
		return BodyFormat::JSON;
	}
	// Pick the highest-q recognized type among json / ndjson / csv / parquet / arrow.
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
		// Prefer more specific non-wildcard when q ties: ndjson/csv/parquet/arrow over */* json.
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

//! RFC 4180-style CSV field escape (quote when needed).
string CsvEscapeField(const string &input) {
	bool need_quote = false;
	for (unsigned char c : input) {
		if (c == '"' || c == ',' || c == '\n' || c == '\r') {
			need_quote = true;
			break;
		}
	}
	if (!need_quote) {
		return input;
	}
	string out;
	out.reserve(input.size() + 2);
	out += '"';
	for (unsigned char c : input) {
		if (c == '"') {
			out += "\"\"";
		} else {
			out += static_cast<char>(c);
		}
	}
	out += '"';
	return out;
}

//! Cell text for CSV: null → empty; scalars ToString; nested → JSON text.
string ValueToCsvCell(const Value &value) {
	if (value.IsNull()) {
		return "";
	}
	auto id = value.type().id();
	if (id == LogicalTypeId::LIST || id == LogicalTypeId::STRUCT || id == LogicalTypeId::MAP) {
		return ValueToJson(value);
	}
	return value.ToString();
}

string SerializeRowsJsonArray(const vector<string> &names, const vector<idx_t> &data_cols,
                              const vector<vector<Value>> &rows) {
	string body = "[";
	bool first_row = true;
	for (auto &cols : rows) {
		if (!first_row) {
			body += ",";
		}
		first_row = false;
		body += "{";
		bool first_col = true;
		for (auto col : data_cols) {
			if (!first_col) {
				body += ",";
			}
			first_col = false;
			body += "\"" + QuackapiJsonEscape(names[col]) + "\":" + ValueToJson(cols[col]);
		}
		body += "}";
	}
	body += "]";
	return body;
}

//! Single-row JSON object (ENVELOPE object). Caller guarantees rows.size() == 1.
string SerializeRowJsonObject(const vector<string> &names, const vector<idx_t> &data_cols, const vector<Value> &cols) {
	string body = "{";
	bool first_col = true;
	for (auto col : data_cols) {
		if (!first_col) {
			body += ",";
		}
		first_col = false;
		body += "\"" + QuackapiJsonEscape(names[col]) + "\":" + ValueToJson(cols[col]);
	}
	body += "}";
	return body;
}

string EmptyResultBody(const QuackapiRoute &route) {
	if (!route.empty_body.empty()) {
		return route.empty_body;
	}
	return "{\"detail\":\"Not Found\"}";
}

string SerializeRowsNdjson(const vector<string> &names, const vector<idx_t> &data_cols,
                           const vector<vector<Value>> &rows) {
	string body;
	for (auto &cols : rows) {
		body += "{";
		bool first_col = true;
		for (auto col : data_cols) {
			if (!first_col) {
				body += ",";
			}
			first_col = false;
			body += "\"" + QuackapiJsonEscape(names[col]) + "\":" + ValueToJson(cols[col]);
		}
		body += "}\n";
	}
	return body;
}

string SerializeRowsCsv(const vector<string> &names, const vector<idx_t> &data_cols,
                        const vector<vector<Value>> &rows) {
	string body;
	// header
	for (idx_t i = 0; i < data_cols.size(); i++) {
		if (i > 0) {
			body += ",";
		}
		body += CsvEscapeField(names[data_cols[i]]);
	}
	body += "\n";
	for (auto &cols : rows) {
		for (idx_t i = 0; i < data_cols.size(); i++) {
			if (i > 0) {
				body += ",";
			}
			auto col = data_cols[i];
			string cell;
			if (col < cols.size()) {
				cell = ValueToCsvCell(cols[col]);
			}
			body += CsvEscapeField(cell);
		}
		body += "\n";
	}
	return body;
}

//! Shared: TEMP table + Appender fill for binary serdes (parquet / arrow).
void FillTempTableForSerdes(Connection &con, const string &table, const vector<string> &names,
                            const vector<LogicalType> &types, const vector<idx_t> &data_cols,
                            const vector<vector<Value>> &rows, const char *label) {
	string create_sql = "CREATE TEMP TABLE " + table + " (";
	for (idx_t i = 0; i < data_cols.size(); i++) {
		if (i > 0) {
			create_sql += ", ";
		}
		auto c = data_cols[i];
		create_sql += KeywordHelper::WriteOptionallyQuoted(names[c]);
		create_sql += " ";
		create_sql += types[c].ToString();
	}
	create_sql += ")";
	auto create_res = con.Query(create_sql);
	if (create_res->HasError()) {
		throw InvalidInputException("%s serialize CREATE: %s", label, create_res->GetError());
	}

	if (!rows.empty()) {
		Appender appender(con, table);
		for (auto &cols : rows) {
			appender.BeginRow();
			for (auto c : data_cols) {
				if (c < cols.size()) {
					appender.Append(cols[c]);
				} else {
					appender.Append(Value());
				}
			}
			appender.EndRow();
		}
		appender.Close();
	}
}

//! OS temp dir + basename for parquet/arrow scratch files. Never hardcode /tmp
//! (Windows CI has no /tmp). Paths use forward slashes for SQL COPY literals.
string SerdesTempFilePath(FileSystem &fs, const string &filename) {
	string dir;
#if defined(_WIN32) || defined(WIN32)
	const char *t = std::getenv("TEMP");
	if (!t || !t[0]) {
		t = std::getenv("TMP");
	}
	dir = (t && t[0]) ? string(t) : string(".");
#else
	const char *t = std::getenv("TMPDIR");
	dir = (t && t[0]) ? string(t) : string("/tmp");
#endif
	string path = fs.JoinPath(dir, filename);
	return StringUtil::Replace(path, "\\", "/");
}

string SqlQuotePath(const string &path) {
	return StringUtil::Replace(path, "'", "''");
}

//! Read path bytes then delete the file. On open failure, best-effort remove.
string ReadAndRemoveFileBytes(Connection &con, const string &path) {
	auto &fs = FileSystem::GetFileSystem(*con.context);
	unique_ptr<FileHandle> handle;
	try {
		handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	} catch (...) {
		fs.TryRemoveFile(path);
		throw;
	}
	idx_t file_size = handle->GetFileSize();
	string body;
	if (file_size > 0) {
		// mutable buffer — data_ptr_cast rejects const char* from string::data()
		vector<data_t> buf(file_size);
		handle->Read(buf.data(), file_size);
		body.assign(reinterpret_cast<const char *>(buf.data()), file_size);
	}
	handle->Close();
	fs.TryRemoveFile(path);
	return body;
}

//! Serialize result rows as a Parquet file body (magic "PAR1"). Uses a unique
//! TEMP table + COPY TO (FORMAT PARQUET) + read bytes, then cleans up.
string SerializeRowsParquet(Connection &con, const vector<string> &names, const vector<LogicalType> &types,
                            const vector<idx_t> &data_cols, const vector<vector<Value>> &rows) {
	if (data_cols.empty()) {
		throw InvalidInputException("parquet serialize: no data columns");
	}
	auto &fs = FileSystem::GetFileSystem(*con.context);
	string uid = StringUtil::Replace(UUID::ToString(UUIDv7::GenerateRandomUUID()), "-", "");
	string table = "qa_parquet_" + uid;
	string path = SerdesTempFilePath(fs, table + ".parquet");

	FillTempTableForSerdes(con, table, names, types, data_cols, rows, "parquet");

	auto copy_res = con.Query("COPY " + table + " TO '" + SqlQuotePath(path) + "' (FORMAT PARQUET)");
	if (copy_res->HasError()) {
		con.Query("DROP TABLE IF EXISTS " + table);
		fs.TryRemoveFile(path);
		throw InvalidInputException("parquet serialize COPY: %s", copy_res->GetError());
	}
	con.Query("DROP TABLE IF EXISTS " + table);
	return ReadAndRemoveFileBytes(con, path);
}

//! Ensure community nanoarrow is available (LOAD, else INSTALL FROM community).
void EnsureNanoarrowLoaded(Connection &con) {
	auto load = con.Query("LOAD nanoarrow");
	if (!load->HasError()) {
		return;
	}
	auto inst = con.Query("INSTALL nanoarrow FROM community");
	if (inst->HasError()) {
		throw InvalidInputException(
		    "arrow serialize: nanoarrow not available (%s). INSTALL nanoarrow FROM community and retry.",
		    inst->GetError());
	}
	load = con.Query("LOAD nanoarrow");
	if (load->HasError()) {
		throw InvalidInputException("arrow serialize: could not LOAD nanoarrow: %s", load->GetError());
	}
}

//! Serialize result rows as Arrow IPC stream bytes (nanoarrow FORMAT ARROWS).
//! Magic is 0xFFFFFFFF stream continuation (not ARROW1 file). Content-Type:
//! application/vnd.apache.arrow.stream. Same TEMP+COPY+read pattern as parquet.
string SerializeRowsArrow(Connection &con, const vector<string> &names, const vector<LogicalType> &types,
                          const vector<idx_t> &data_cols, const vector<vector<Value>> &rows) {
	if (data_cols.empty()) {
		throw InvalidInputException("arrow serialize: no data columns");
	}
	EnsureNanoarrowLoaded(con);
	auto &fs = FileSystem::GetFileSystem(*con.context);
	string uid = StringUtil::Replace(UUID::ToString(UUIDv7::GenerateRandomUUID()), "-", "");
	string table = "qa_arrow_" + uid;
	string path = SerdesTempFilePath(fs, table + ".arrows");

	FillTempTableForSerdes(con, table, names, types, data_cols, rows, "arrow");

	auto copy_res = con.Query("COPY " + table + " TO '" + SqlQuotePath(path) + "' (FORMAT ARROWS)");
	if (copy_res->HasError()) {
		con.Query("DROP TABLE IF EXISTS " + table);
		fs.TryRemoveFile(path);
		throw InvalidInputException("arrow serialize COPY: %s", copy_res->GetError());
	}
	con.Query("DROP TABLE IF EXISTS " + table);
	return ReadAndRemoveFileBytes(con, path);
}

void SetInternalError(duckdb_httplib::Response &res, const string &server_side_detail) {
	// Return DuckDB's diagnostic so clients can diagnose route execution failures.
	fprintf(stderr, "quackapi: internal error: %s\n", server_side_detail.c_str());
	SetJson(res, 500, "{\"detail\":\"" + QuackapiJsonEscape(server_side_detail) + "\"}");
}

} // namespace duckdb
