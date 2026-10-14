#pragma once

#include "duckdb/common/types.hpp"
#include "httplib.hpp"
#include <memory>

namespace duckdb {

class Connection;
struct QuackapiRoute;
class DataChunk;

enum class ResponseMode { JSON, HTML, TEXT };
enum class BodyFormat { JSON, NDJSON, CSV };
enum class JsonRowMode { ARRAY, OBJECT, NDJSON };

//! Bounded accumulator shared by streaming encoders. Each append is atomic:
//! once the next append would exceed the limit, it is rejected and Data() is
//! empty so callers cannot accidentally send a truncated response.
class QuackapiResponseWriter {
public:
	explicit QuackapiResponseWriter(idx_t max_bytes);
	bool Append(const char *data, idx_t length);
	bool Append(const string &data);
	idx_t BytesWritten() const;
	bool ExceededLimit() const;
	const string &Data() const;
	string TakeData();

private:
	idx_t max_bytes;
	idx_t bytes_written;
	bool exceeded_limit;
	string data;
};

//! Vectorized JSON row encoder. A native DuckDB json_object expression is
//! bound once for the result schema and evaluated against each result chunk.
class QuackapiJsonChunkEncoder {
public:
	QuackapiJsonChunkEncoder(Connection &con, const vector<string> &names, const vector<LogicalType> &types,
	                         const vector<idx_t> &data_cols, JsonRowMode mode, QuackapiResponseWriter &writer);
	~QuackapiJsonChunkEncoder();
	QuackapiJsonChunkEncoder(QuackapiJsonChunkEncoder &&) noexcept;
	QuackapiJsonChunkEncoder &operator=(QuackapiJsonChunkEncoder &&) noexcept;
	QuackapiJsonChunkEncoder(const QuackapiJsonChunkEncoder &) = delete;
	QuackapiJsonChunkEncoder &operator=(const QuackapiJsonChunkEncoder &) = delete;

	void AppendChunk(DataChunk &chunk);
	idx_t RowCount() const;
	void Finish();

private:
	class State;
	unique_ptr<State> state;
};

//! DuckDB-native CSV writer for result chunks. All output, including the
//! header and row terminators, flows through the same bounded response writer.
class QuackapiCsvChunkEncoder {
public:
	QuackapiCsvChunkEncoder(Connection &con, const vector<string> &names, const vector<LogicalType> &types,
	                        const vector<idx_t> &data_cols, QuackapiResponseWriter &writer);
	~QuackapiCsvChunkEncoder();
	QuackapiCsvChunkEncoder(QuackapiCsvChunkEncoder &&) noexcept;
	QuackapiCsvChunkEncoder &operator=(QuackapiCsvChunkEncoder &&) noexcept;
	QuackapiCsvChunkEncoder(const QuackapiCsvChunkEncoder &) = delete;
	QuackapiCsvChunkEncoder &operator=(const QuackapiCsvChunkEncoder &) = delete;

	void AppendChunk(DataChunk &chunk);
	void Finish();

private:
	class State;
	unique_ptr<State> state;
};

bool IsSpecialResponseColumn(const string &name);
void SetJson(duckdb_httplib::Response &res, int status, const string &body);
void SetInternalError(duckdb_httplib::Response &res, const string &server_side_detail);
ResponseMode ResponseModeFor(const vector<string> &names);
BodyFormat ResolveBodyFormat(const QuackapiRoute &route, const duckdb_httplib::Request &req);
string EmptyResultBody(const QuackapiRoute &route);

} // namespace duckdb
