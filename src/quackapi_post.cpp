#include "quackapi_post.hpp"
#include "quackapi_post_executor.hpp"
#include "quackapi_post_provider.hpp"
#include "quackapi_post_result.hpp"

#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

namespace {

struct QuackapiPostTableBindData : public TableFunctionData {
	idx_t url_idx;
	idx_t body_idx;
	optional_idx content_type_idx;
	optional_idx headers_idx;
	vector<string> input_names;
	vector<LogicalType> input_types;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<QuackapiPostTableBindData>(*this);
	}
};

struct QuackapiPostTableGlobalState : public GlobalTableFunctionState {
	idx_t MaxThreads() const override {
		return 1;
	}
};

struct QuackapiPostTableLocalState : public LocalTableFunctionState {
	idx_t input_offset = 0;
};

bool InitializePostRequest(const Value &url, const Value &body, const Value &content_type, const Value &headers,
                           QuackapiPostRequest &request) {
	if (url.IsNull()) {
		return false;
	}
	request.url = url.ToString();
	request.body = body.IsNull() ? string() : body.ToString();
	request.content_type = content_type.IsNull() ? "application/json" : content_type.ToString();
	request.headers = QuackapiHttpHeadersFromValue(headers);
	return true;
}

unique_ptr<FunctionData> BindPostTable(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	if (input.input_table_names.size() != input.input_table_types.size()) {
		throw BinderException("quackapi_post(table) could not resolve the input table columns");
	}
	auto bind_data = make_uniq<QuackapiPostTableBindData>();
	bind_data->input_names = input.input_table_names;
	bind_data->input_types = input.input_table_types;
	optional_idx url_idx;
	optional_idx body_idx;
	for (idx_t i = 0; i < input.input_table_names.size(); i++) {
		const auto name = StringUtil::Lower(input.input_table_names[i]);
		auto set_column = [&](optional_idx &column_idx) {
			if (column_idx.IsValid()) {
				throw BinderException(
				    "quackapi_post(table) requires unique input columns named url, body, content_type, and headers");
			}
			column_idx = i;
		};
		if (name == "url") {
			set_column(url_idx);
		} else if (name == "body") {
			set_column(body_idx);
		} else if (name == "content_type") {
			set_column(bind_data->content_type_idx);
		} else if (name == "headers") {
			set_column(bind_data->headers_idx);
		} else if (name == "response") {
			throw BinderException(
			    "quackapi_post(table) cannot append response: the input already has a response column");
		}
	}
	if (!url_idx.IsValid() || !body_idx.IsValid()) {
		throw BinderException("quackapi_post(table) requires input columns named url and body");
	}
	if (input.input_table_types[url_idx.GetIndex()] != LogicalType::VARCHAR) {
		throw BinderException("quackapi_post(table) requires url to be VARCHAR");
	}
	const auto body_type = input.input_table_types[body_idx.GetIndex()];
	if (body_type != LogicalType::VARCHAR && body_type != LogicalType::JSON()) {
		throw BinderException("quackapi_post(table) requires body to be VARCHAR or JSON");
	}
	if (bind_data->content_type_idx.IsValid() &&
	    input.input_table_types[bind_data->content_type_idx.GetIndex()] != LogicalType::VARCHAR) {
		throw BinderException("quackapi_post(table) requires content_type to be VARCHAR");
	}
	const auto headers_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	if (bind_data->headers_idx.IsValid() &&
	    input.input_table_types[bind_data->headers_idx.GetIndex()] != headers_type) {
		throw BinderException("quackapi_post(table) requires headers to be MAP(VARCHAR, VARCHAR)");
	}
	bind_data->url_idx = url_idx.GetIndex();
	bind_data->body_idx = body_idx.GetIndex();
	return_types = bind_data->input_types;
	names = bind_data->input_names;
	return_types.emplace_back(QuackapiHttpFetchResultType());
	names.emplace_back("response");
	return bind_data;
}

unique_ptr<GlobalTableFunctionState> InitPostTableGlobal(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<QuackapiPostTableGlobalState>();
}

unique_ptr<LocalTableFunctionState> InitPostTableLocal(ExecutionContext &, TableFunctionInitInput &,
                                                       GlobalTableFunctionState *) {
	return make_uniq<QuackapiPostTableLocalState>();
}

OperatorResultType PostTableInOut(ExecutionContext &context, TableFunctionInput &data, DataChunk &input,
                                  DataChunk &output) {
	if (context.client.IsInterrupted()) {
		throw InterruptException();
	}
	if (input.size() == 0) {
		output.SetCardinality(0);
		return OperatorResultType::NEED_MORE_INPUT;
	}
	auto &bind_data = data.bind_data->Cast<QuackapiPostTableBindData>();
	auto &local_state = data.local_state->Cast<QuackapiPostTableLocalState>();
	const auto concurrency = QuackapiPostExecutor::EffectiveConcurrency(context.client);
	const auto batch_size = MinValue<idx_t>(concurrency, input.size() - local_state.input_offset);
	vector<QuackapiPostRequest> requests(batch_size);
	vector<bool> skip(batch_size, false);
	const Value default_content_type(LogicalType::VARCHAR);
	const Value default_headers(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
	for (idx_t i = 0; i < batch_size; i++) {
		const auto row = local_state.input_offset + i;
		const auto url = input.data[bind_data.url_idx].GetValue(row);
		const auto body = input.data[bind_data.body_idx].GetValue(row);
		Value content_type = default_content_type;
		if (bind_data.content_type_idx.IsValid()) {
			content_type = input.data[bind_data.content_type_idx.GetIndex()].GetValue(row);
		}
		Value headers = default_headers;
		if (bind_data.headers_idx.IsValid()) {
			headers = input.data[bind_data.headers_idx.GetIndex()].GetValue(row);
		}
		skip[i] = !InitializePostRequest(url, body, content_type, headers, requests[i]);
	}
	auto responses = QuackapiPostExecutor::Execute(context.client, std::move(requests), skip);
	const auto result_column = output.ColumnCount() - 1;
	const auto result_type = QuackapiHttpFetchResultType();
	SelectionVector selection(batch_size);
	for (idx_t i = 0; i < batch_size; i++) {
		selection.set_index(i, local_state.input_offset + i);
	}
	for (idx_t i = 0; i < batch_size; i++) {
		if (skip[i]) {
			output.data[result_column].SetValue(i, Value(result_type));
		} else {
			auto response = std::move(responses[i]);
			output.data[result_column].SetValue(i, QuackapiHttpFetchResultValue(response));
		}
	}
	for (idx_t column = 0; column < input.ColumnCount(); column++) {
		output.data[column].Slice(input.data[column], selection, batch_size);
	}
	output.SetCardinality(batch_size);
	local_state.input_offset += batch_size;
	if (local_state.input_offset < input.size()) {
		return OperatorResultType::HAVE_MORE_OUTPUT;
	}
	local_state.input_offset = 0;
	return OperatorResultType::NEED_MORE_INPUT;
}

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
	const Value default_content_type(LogicalType::VARCHAR);
	const Value default_headers(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
	for (idx_t i = 0; i < count; i++) {
		const auto url = args.data[0].GetValue(i);
		const auto body = args.data[1].GetValue(i);
		Value content_type = default_content_type;
		if (args.ColumnCount() > 2) {
			content_type = args.data[2].GetValue(i);
		}
		Value headers = default_headers;
		if (args.ColumnCount() > 3) {
			headers = args.data[3].GetValue(i);
		}
		skip[i] = !InitializePostRequest(url, body, content_type, headers, requests[i]);
	}

	auto responses = QuackapiPostExecutor::Execute(context, std::move(requests), skip);

	for (idx_t i = 0; i < count; i++) {
		if (skip[i]) {
			result.SetValue(i, Value(QuackapiHttpFetchResultType()));
			continue;
		}
		// Move each response out of the batch before copying it into the output
		// vector. Keeping the whole responses vector alive here duplicates every
		// body while the result vector is being populated.
		auto response = std::move(responses[i]);
		result.SetValue(i, QuackapiHttpFetchResultValue(response));
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

	TableFunction post_table("quackapi_post", {LogicalType::TABLE}, nullptr, BindPostTable, InitPostTableGlobal,
	                         InitPostTableLocal);
	post_table.in_out_function = PostTableInOut;
	loader.RegisterFunction(post_table);
}

} // namespace duckdb
