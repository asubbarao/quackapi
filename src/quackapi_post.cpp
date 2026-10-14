#include "quackapi_post.hpp"
#include "quackapi_post_executor.hpp"
#include "quackapi_post_provider.hpp"

#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

namespace {

void PostScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &db = *state.GetContext().db;
	const auto count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);

	vector<bool> skip(count, false);
	vector<string> urls(count);
	vector<string> bodies(count);
	vector<string> content_types(count);
	vector<unordered_map<string, string>> headers(count);
	for (idx_t i = 0; i < count; i++) {
		auto url = args.data[0].GetValue(i);
		if (url.IsNull()) {
			skip[i] = true;
			continue;
		}
		urls[i] = url.ToString();
		auto body = args.data[1].GetValue(i);
		bodies[i] = body.IsNull() ? string() : body.ToString();
		content_types[i] = "application/json";
		if (args.ColumnCount() > 2) {
			auto content_type = args.data[2].GetValue(i);
			if (!content_type.IsNull()) {
				content_types[i] = content_type.ToString();
			}
		}
		if (args.ColumnCount() > 3) {
			headers[i] = QuackapiHttpHeadersFromValue(args.data[3].GetValue(i));
		}
	}

	vector<QuackapiHttpFetchResult> responses(count);
	QuackapiPostExecutor::Execute(count, skip, responses, [&](idx_t i) {
		return QuackapiPostProvider::Post(db, urls[i], bodies[i], content_types[i], headers[i]);
	});

	for (idx_t i = 0; i < count; i++) {
		result.SetValue(i, skip[i] ? Value(QuackapiHttpFetchResultType()) : QuackapiHttpFetchResultValue(responses[i]));
	}
}

} // namespace

void RegisterQuackapiPostFunctions(ExtensionLoader &loader) {
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
