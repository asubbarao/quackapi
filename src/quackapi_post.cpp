#include "quackapi_post.hpp"
#include "quackapi_post_executor.hpp"
#include "quackapi_post_provider.hpp"
#include "quackapi_post_result.hpp"

#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

namespace {

void ValidatePostConcurrency(ClientContext &, SetScope, Value &value) {
	if (value.GetValue<int64_t>() < 0) {
		throw InvalidInputException("quackapi_post_concurrency must be 0 or a positive integer");
	}
}

void PostScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	const auto count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);

	vector<bool> skip(count, false);
	vector<QuackapiPostRequest> requests(count);
	for (idx_t i = 0; i < count; i++) {
		auto url = args.data[0].GetValue(i);
		if (url.IsNull()) {
			skip[i] = true;
			continue;
		}
		requests[i].url = url.ToString();
		auto body = args.data[1].GetValue(i);
		requests[i].body = body.IsNull() ? string() : body.ToString();
		requests[i].content_type = "application/json";
		if (args.ColumnCount() > 2) {
			auto content_type = args.data[2].GetValue(i);
			if (!content_type.IsNull()) {
				requests[i].content_type = content_type.ToString();
			}
		}
		if (args.ColumnCount() > 3) {
			requests[i].headers = QuackapiHttpHeadersFromValue(args.data[3].GetValue(i));
		}
	}

	auto responses = QuackapiPostExecutor::Execute(context, std::move(requests), skip);

	for (idx_t i = 0; i < count; i++) {
		result.SetValue(i, skip[i] ? Value(QuackapiHttpFetchResultType()) : QuackapiHttpFetchResultValue(responses[i]));
	}
}

} // namespace

void RegisterQuackapiPostFunctions(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("quackapi_post_concurrency",
	                          "Maximum concurrent outbound quackapi_post requests for this database. "
	                          "0 uses the database thread count (minimum 1).",
	                          LogicalType::BIGINT, Value::BIGINT(0), ValidatePostConcurrency, SetScope::GLOBAL);
	const auto header_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	const auto result_type = QuackapiHttpFetchResultType();

	// POST changes remote state, so all overloads must stay volatile.
	ScalarFunctionSet post_set("quackapi_post");
	ScalarFunction post2("quackapi_post", {LogicalType::VARCHAR, LogicalType::VARCHAR}, result_type, PostScalar);
	post2.stability = FunctionStability::VOLATILE;
	ScalarFunction post2j("quackapi_post", {LogicalType::VARCHAR, LogicalType::JSON()}, result_type, PostScalar);
	post2j.stability = FunctionStability::VOLATILE;
	ScalarFunction post3("quackapi_post", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                     result_type, PostScalar);
	post3.stability = FunctionStability::VOLATILE;
	ScalarFunction post4("quackapi_post",
	                     {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, header_type}, result_type,
	                     PostScalar);
	post4.stability = FunctionStability::VOLATILE;
	post_set.AddFunction(post2);
	post_set.AddFunction(post2j);
	post_set.AddFunction(post3);
	post_set.AddFunction(post4);
	loader.RegisterFunction(post_set);
}

} // namespace duckdb
