#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

//! Shared helpers — one definition (not six byte-identical Trims / two JsonEscapes).

//! Trim whitespace and trailing ';'.
string QuackapiTrim(const string &input);

//! Comment-aware QuackapiTrim for hand-rolled DDL grammars: also skips leading
//! '--' line comments and nestable '/* */' block comments before the next
//! keyword is matched. Every quackapi ParserExtension::parse_function receives
//! the *raw* split-out statement text — DuckDB tokenizes the whole script to
//! find statement boundaries (comment-aware) but only whitespace-trims each
//! resulting segment before handing it to the extension (see SplitQueries in
//! duckdb/src/parser/parser.cpp), so a comment sitting at a clause boundary is
//! still attached. A grammar that only calls QuackapiTrim there misses its
//! keyword and silently declines the statement, and DuckDB then reports its own
//! confusing syntax error instead. Use this wherever a DDL grammar advances past
//! whitespace to the next token.
string QuackapiDdlTrim(const string &input);

//! JSON string escape including control bytes as \u00XX (auth + OpenAPI + responses).
string QuackapiJsonEscape(const string &input);

//===--------------------------------------------------------------------===//
// DDL apply-table-function shell (CREATE/DROP ROUTE|GROUP|AUTH|… plan→exec)
//
// Bind/exec *payloads* stay per-noun (different fields + registry side effects).
// These helpers only kill the repeated wiring:
//   return_types = {VARCHAR} / names = {"status"}
//   TableFunction(name, arg_types, exec, bind)
//   plan.requires_valid_transaction = false + QUERY_RESULT
//   one-shot status row emit
//===--------------------------------------------------------------------===//

inline void BindStatusColumn(vector<LogicalType> &return_types, vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("status");
}

inline TableFunction MakeApplyDdlFunction(const char *name, vector<LogicalType> arg_types, table_function_t function,
                                          table_function_bind_t bind) {
	return TableFunction(name, std::move(arg_types), function, bind);
}

inline void FinishDdlPlan(ParserExtensionPlanResult &result) {
	result.requires_valid_transaction = false;
	result.return_type = StatementReturnType::QUERY_RESULT;
}

//! Emit the single VARCHAR status row and mark the apply TF finished.
inline void EmitOneShotStatus(DataChunk &output, bool &finished, const string &message) {
	output.SetValue(0, 0, Value(message));
	output.SetCardinality(1);
	finished = true;
}

} // namespace duckdb
