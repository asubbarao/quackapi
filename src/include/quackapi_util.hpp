#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/query_result.hpp"

namespace duckdb {

//! Shared helpers — one definition (not six byte-identical Trims / two JsonEscapes).

//! Trim whitespace and trailing ';'.
string QuackapiTrim(const string &input);

//! JSON string escape including control bytes as \u00XX (auth + OpenAPI + responses).
string QuackapiJsonEscape(const string &input);

//! An exec route's handler is exactly one named parameter (`AS $sql`). The
//! request binds that parameter and quackapi executes its text as SQL on the
//! request connection instead of preparing the handler. Returns the parameter
//! name. $claims_* names are server-bound, never request SQL, so they do not
//! qualify.
bool QuackapiExecRouteParam(const string &handler_sql, string &param_name);

//! Collect native query rows as a LIST<VARIANT> with one fixed STRUCT type per result.
//! The byte limit measures DuckDB's native binary serialization of the complete list,
//! including list, schema and row overhead; it is not an estimate of heap usage.
struct QuackapiTypedResult {
	Value rows;
	Value columns;
	QuackapiTypedResult();
};

class QuackapiTypedRowsTooLarge : public InvalidInputException {
public:
	explicit QuackapiTypedRowsTooLarge(const string &message) : InvalidInputException(message) {
	}
};

class QuackapiTypedRowCollector {
public:
	QuackapiTypedRowCollector(vector<string> names, vector<LogicalType> types, idx_t max_bytes);
	void AppendRow(vector<Value> values);
	void AppendChunk(const DataChunk &chunk);
	QuackapiTypedResult Finish();

private:
	idx_t column_count;
	LogicalType row_type;
	vector<Value> rows;
	idx_t max_bytes;
	idx_t empty_rows_bytes;
	idx_t empty_result_bytes;
	idx_t row_payload_bytes = 0;
};

QuackapiTypedResult QuackapiTypedRowsFromResult(QueryResult &result, idx_t max_bytes);

//===--------------------------------------------------------------------===//
// DDL from the 2.0 token view
//
// DuckDB 2.0 hands a parser extension the tokens of the statement it could not
// parse (comments dropped, every literal byte for byte) instead of the query
// text. The statement is rebuilt from them - one space between tokens, none
// inside f(x), t.c, x::t, T[], $p - and the string grammar of each noun parses
// that. Literals and quoted identifiers come back verbatim; only whitespace and
// comments between tokens change. The rebuilt text is re-tokenized and must give
// the same tokens, and two adjacent string literals are refused (whether they
// concatenate depends on a newline the token view drops).
//===--------------------------------------------------------------------===//

using quackapi_string_parse_t = ParserExtensionParseResult (*)(ParserExtensionInfo *, const string &);

//! The statement at the head of tokens as SQL text, and how many tokens it spans (excluding its ';').
bool QuackapiTokensToSql(const vector<SimpleToken> &tokens, idx_t &statement_tokens, string &sql, string &error);

ParserExtensionParseResult QuackapiParseTokens(ParserExtensionInfo *info, const vector<SimpleToken> &tokens,
                                               quackapi_string_parse_t parse);

template <quackapi_string_parse_t PARSE>
ParserExtensionParseResult QuackapiTokenParse(ParserExtensionInfo *info, const vector<SimpleToken> &tokens) {
	return QuackapiParseTokens(info, tokens, PARSE);
}

//===--------------------------------------------------------------------===//
// Function signatures in 2.0
//===--------------------------------------------------------------------===//

//! Optional named parameters are a typed "**kwargs" schema on the signature: a call that omits one leaves it out
//! of input.named_parameters, as before.
inline void AddNamedParameter(TableFunction &fn, const char *name, const LogicalType &type) {
	auto &signature = fn.GetSignature();
	auto add = [&](TypedKwargs &options) {
		options.Add(name, type);
	};
	if (signature.GetTypedKwargs()) {
		signature.ExtendTypedKwargs(add);
	} else {
		signature.WithTypedKwargs("options", add);
	}
}

//! Reuse an overload's implementation and typed keyword schema with different positional parameters.
inline TableFunction WithPositionalArguments(TableFunction fn, vector<LogicalType> arguments) {
	FunctionSignature signature(std::move(arguments), fn.GetSignature().GetReturnType());
	auto kwargs = fn.GetSignature().GetTypedKwargs();
	if (kwargs) {
		signature.AddTypedKwargs("options", *kwargs);
	}
	fn.GetSignature() = std::move(signature);
	return fn;
}

inline TableFunction WithoutPositionalArguments(TableFunction fn) {
	return WithPositionalArguments(std::move(fn), {});
}

//! DuckDB 2.0 reports an exception from a scalar that is not marked fallible as an INTERNAL error.
inline void RegisterFallible(ExtensionLoader &loader, ScalarFunction function) {
	function.SetFallible();
	loader.RegisterFunction(std::move(function));
}
inline void RegisterFallible(ExtensionLoader &loader, ScalarFunctionSet functions) {
	functions.SetFallible();
	loader.RegisterFunction(std::move(functions));
}

//! Value::DefaultTryCastAs with an out-parameter (2.0 returns an optional).
inline bool QuackapiTryCast(const Value &value, const LogicalType &type, Value &out, string *error = nullptr) {
	auto cast = value.DefaultTryCastAs(type, error);
	if (!cast) {
		return false;
	}
	out = std::move(*cast);
	return true;
}

//! Result column names as plain strings, for the serializers.
inline vector<string> QuackapiNames(const vector<Identifier> &names) {
	vector<string> result;
	result.reserve(names.size());
	for (auto &name : names) {
		result.push_back(name.GetIdentifierName());
	}
	return result;
}

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
	output.data[0].SetValue(0, Value(message));
	output.SetChildCardinality(1);
	finished = true;
}

} // namespace duckdb
