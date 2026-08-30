#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

#include <utility>

namespace duckdb {

//! Thin libpq path for pure-HTTP routes that hit Postgres — same shape as
//! FastAPI+psycopg: bind params, fresh PQexecParams, JSON rows. Bypasses DuckDB
//! ATTACH/scanner for that request (optional; when pg_dsn is set on serve or
//! quackapi_request / SET quackapi_pg_dsn).
//!
//! Returns true and fills json_body on success. false → caller uses DuckDB path.
bool QuackapiTryPgNative(const string &dsn, const string &handler_sql,
                         const case_insensitive_map_t<std::pair<string, string>> &provided, string &json_body,
                         string &err_out);

} // namespace duckdb
