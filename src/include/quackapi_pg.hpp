#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

#include <utility>
#include <cstdint>

namespace duckdb {

//! Thin libpq path for pure-HTTP routes that hit Postgres — same shape as
//! FastAPI+psycopg: bind params, fresh PQexecParams, JSON rows. Bypasses DuckDB
//! ATTACH/scanner for that request (optional; when pg_dsn is set on serve or
//! quackapi_request / SET quackapi_pg_dsn).
//!
//! Whether the native libpq execution path completed, is proven inapplicable,
//! or failed after PostgreSQL transport/execution began. Callers may fall back
//! to DuckDB only for NOT_APPLICABLE: replaying a failed mutating PG command
//! against another backend is unsafe.
enum class QuackapiPgNativeResult : uint8_t {
	NOT_APPLICABLE,
	SUCCESS,
	FAILED,
};

//! A bounded native libpq execution. `max_response_bytes` caps JSON assembly
//! while rows arrive in single-row mode. On FAILED, err_out is a sanitized
//! stable category; "PostgreSQL deadline exceeded" maps to HTTP 504.
QuackapiPgNativeResult QuackapiTryPgNative(const string &dsn, const string &handler_sql,
                                           const case_insensitive_map_t<std::pair<string, string>> &provided,
                                           idx_t max_response_bytes, string &json_body, string &err_out);

} // namespace duckdb
