#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "httplib.hpp"

namespace duckdb {

class Connection;
struct QuackapiRoute;

enum class ResponseMode { JSON, HTML, TEXT };
enum class BodyFormat { JSON, NDJSON, CSV, PARQUET, ARROW };

string ValueToJson(const Value &value);
bool IsSpecialResponseColumn(const string &name);
void SetJson(duckdb_httplib::Response &res, int status, const string &body);
void SetInternalError(duckdb_httplib::Response &res, const string &server_side_detail);
ResponseMode ResponseModeFor(const vector<string> &names);
BodyFormat ResolveBodyFormat(const QuackapiRoute &route, const duckdb_httplib::Request &req);
string EmptyResultBody(const QuackapiRoute &route);
string SerializeRowsJsonArray(const vector<string> &names, const vector<idx_t> &data_cols,
                              const vector<vector<Value>> &rows);
string SerializeRowJsonObject(const vector<string> &names, const vector<idx_t> &data_cols, const vector<Value> &cols);
string SerializeRowsNdjson(const vector<string> &names, const vector<idx_t> &data_cols,
                           const vector<vector<Value>> &rows);
string SerializeRowsCsv(const vector<string> &names, const vector<idx_t> &data_cols, const vector<vector<Value>> &rows);
string SerializeRowsParquet(Connection &con, const vector<string> &names, const vector<LogicalType> &types,
                            const vector<idx_t> &data_cols, const vector<vector<Value>> &rows);
string SerializeRowsArrow(Connection &con, const vector<string> &names, const vector<LogicalType> &types,
                          const vector<idx_t> &data_cols, const vector<vector<Value>> &rows);

} // namespace duckdb
