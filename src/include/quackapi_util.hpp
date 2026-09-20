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

//! JSON string escape including control bytes as \u00XX (auth + OpenAPI + responses).
string QuackapiJsonEscape(const string &input);

//===--------------------------------------------------------------------===//
// Token view → statement text
//
// A parse_function is handed the token view of everything the PEG parser could
// not consume, which may hold several statements, and must report how many of
// those tokens it claimed. The DDL parsers read text, so rebuild the leading
// statement from its tokens and carry the count alongside it.
//===--------------------------------------------------------------------===//

struct QuackapiTokenStatement {
	//! Token texts of the leading statement, re-joined; the terminator is not part of it.
	string query;
	//! Tokens the statement spans, including its ';' or the end-of-input sentinel.
	int64_t consumed_tokens = 0;
};

//! Rebuild the leading statement of `tokens`. Token texts are the exact source bytes, so a
//! single space between them re-tokenizes identically; the one join that must stay tight is
//! '$' with the parameter name after it, which quackapi's own $param scanner looks for.
QuackapiTokenStatement QuackapiStatementFromTokens(const vector<SimpleToken> &tokens);

//! Report to the peeler what the text parser decided: the whole statement on success, nothing
//! when the statement was not ours, and a negative count to raise the extension's own error.
ParserExtensionParseResult QuackapiClaimTokens(ParserExtensionParseResult result, int64_t consumed_tokens);

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

inline void BindStatusColumn(vector<LogicalType> &return_types, vector<Identifier> &names) {
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
