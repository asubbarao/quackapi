#include "quackapi_from_x.hpp"
#include "quackapi_from_x_sql.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/query_result.hpp"

namespace duckdb {

namespace {

//===--------------------------------------------------------------------===//
// sitting_duck bootstrap (INSTALL FROM community + LOAD; idempotent)
//===--------------------------------------------------------------------===//

void EnsureSittingDuck(Connection &con) {
	// LOAD is cheap and idempotent when already present.
	auto load = con.Query("LOAD sitting_duck");
	if (!load->HasError()) {
		return;
	}
	auto inst = con.Query("INSTALL sitting_duck FROM community");
	if (inst->HasError()) {
		throw InvalidInputException("quack_from_x: could not INSTALL sitting_duck FROM community: %s\n"
		                            "sitting_duck is a runtime dependency of quack_from_* extractors.",
		                            inst->GetError().c_str());
	}
	load = con.Query("LOAD sitting_duck");
	if (load->HasError()) {
		throw InvalidInputException("quack_from_x: could not LOAD sitting_duck: %s", load->GetError().c_str());
	}
}

//===--------------------------------------------------------------------===//
// Path expansion: bare directory → framework-specific globs for __REPO__
//===--------------------------------------------------------------------===//

bool LooksLikeGlob(const string &path) {
	return path.find('*') != string::npos || path.find('?') != string::npos;
}

string RtrimSlashes(string p) {
	while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) {
		p.pop_back();
	}
	return p;
}

//! Escape a path for embedding inside a single-quoted SQL string literal.
string EscapeSqlString(const string &s) {
	string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		if (c == '\'') {
			out += "''";
		} else {
			out += c;
		}
	}
	return out;
}

//! Expand a user path argument into the value substituted for __REPO__.
//! framework/kind control default globs when the user passes a bare directory.
string ExpandRepoPath(const string &raw_path, const string &framework, const string &kind) {
	if (raw_path.empty()) {
		throw InvalidInputException("quack_from_x: path must be non-empty");
	}
	string path = RtrimSlashes(raw_path);

	// Caller already supplied a glob or a specific source file — use as-is.
	if (LooksLikeGlob(path)) {
		return path;
	}
	// Specific source file extensions: use as-is.
	auto lower = StringUtil::Lower(path);
	if (StringUtil::EndsWith(lower, ".py") || StringUtil::EndsWith(lower, ".rb") ||
	    StringUtil::EndsWith(lower, ".ts") || StringUtil::EndsWith(lower, ".js") ||
	    StringUtil::EndsWith(lower, ".go")) {
		return path;
	}

	// Bare directory defaults.
	if (framework == "fastapi") {
		return path + "/**/*.py";
	}
	if (framework == "rails") {
		if (kind == "routes") {
			return path + "/config/routes.rb";
		}
		// models SQL embeds '__REPO__/app/models/**/*.rb' itself — pass bare root.
		return path;
	}
	if (framework == "express") {
		// express SQL embeds '__REPO__/**/*.{ts,js}' itself — pass bare root.
		return path;
	}
	if (framework == "gin") {
		return path + "/**/*.go";
	}
	return path;
}

string SubstituteRepo(const string &sql_template, const string &repo_path) {
	// SQL files use the literal token __REPO__ inside string literals.
	return StringUtil::Replace(sql_template, "__REPO__", EscapeSqlString(repo_path));
}

//===--------------------------------------------------------------------===//
// Materialized extraction runner
//===--------------------------------------------------------------------===//

struct FromXFunctionInfo : public TableFunctionInfo {
	string framework;
	string kind; // "routes" | "models"
	FromXFunctionInfo(string framework_p, string kind_p) : framework(std::move(framework_p)), kind(std::move(kind_p)) {
	}
};

struct FromXBindData : public TableFunctionData {
	string framework;
	string kind; // "routes" | "models"
	string path;
	bool is_routes = true;
};

struct FromXGlobalState : public GlobalTableFunctionState {
	vector<vector<Value>> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

void SetRouteReturnTypes(vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR,  LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::UINTEGER, LogicalType::VARCHAR};
	names = {"method", "path", "handler_name", "file", "start_line", "evidence"};
}

void SetModelReturnTypes(vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::BOOLEAN,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::UINTEGER};
	names = {"model_name",  "field_name",   "field_type", "is_required", "is_optional",
	         "has_default", "default_expr", "file",       "field_line"};
}

unique_ptr<FunctionData> FromXBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                   vector<string> &names) {
	auto &info = input.info->Cast<FromXFunctionInfo>();
	auto data = make_uniq<FromXBindData>();
	data->framework = info.framework;
	data->kind = info.kind;
	data->is_routes = (info.kind == "routes");
	data->path = input.inputs[0].GetValue<string>();
	if (data->is_routes) {
		SetRouteReturnTypes(return_types, names);
	} else {
		SetModelReturnTypes(return_types, names);
	}
	return std::move(data);
}

Value CoerceColumn(const Value &v, const LogicalType &want) {
	if (v.IsNull()) {
		return Value(want);
	}
	if (v.type() == want) {
		return v;
	}
	// Best-effort: cast via string/int paths we control.
	try {
		if (want.id() == LogicalTypeId::VARCHAR) {
			return Value(v.ToString());
		}
		if (want.id() == LogicalTypeId::BOOLEAN) {
			if (v.type().id() == LogicalTypeId::BOOLEAN) {
				return v;
			}
			return Value::BOOLEAN(v.GetValue<bool>());
		}
		if (want.id() == LogicalTypeId::UINTEGER) {
			if (v.type().IsIntegral()) {
				return Value::UINTEGER(static_cast<uint32_t>(v.GetValue<int64_t>()));
			}
			return Value::UINTEGER(static_cast<uint32_t>(std::stoul(v.ToString())));
		}
	} catch (...) {
		return Value(want);
	}
	return Value(want);
}

unique_ptr<GlobalTableFunctionState> FromXInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<FromXBindData>();
	auto state = make_uniq<FromXGlobalState>();

	const char *sql_tmpl = quackapi_from_x_sql::Lookup(bind.framework.c_str(), bind.kind.c_str());
	if (!sql_tmpl) {
		throw InternalException("quack_from_x: missing embedded SQL for %s/%s", bind.framework, bind.kind);
	}

	string repo = ExpandRepoPath(bind.path, bind.framework, bind.kind);
	string sql = SubstituteRepo(sql_tmpl, repo);

	Connection con(*context.db);
	EnsureSittingDuck(con);

	auto result = con.Query(sql);
	if (result->HasError()) {
		throw InvalidInputException("quack_from_%s%s(%s): extraction failed: %s", bind.framework,
		                            bind.is_routes ? "" : "_models", bind.path, result->GetError().c_str());
	}

	// Expected column counts for coercion.
	const idx_t ncol = bind.is_routes ? 6 : 9;
	vector<LogicalType> want_types;
	vector<string> want_names;
	if (bind.is_routes) {
		SetRouteReturnTypes(want_types, want_names);
	} else {
		SetModelReturnTypes(want_types, want_names);
	}

	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		for (idx_t r = 0; r < chunk->size(); r++) {
			vector<Value> row;
			row.reserve(ncol);
			for (idx_t c = 0; c < ncol; c++) {
				if (c < chunk->ColumnCount()) {
					row.push_back(CoerceColumn(chunk->GetValue(c, r), want_types[c]));
				} else {
					row.push_back(Value(want_types[c]));
				}
			}
			state->rows.push_back(std::move(row));
		}
	}
	return std::move(state);
}

void FromXExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<FromXGlobalState>();
	if (state.offset >= state.rows.size()) {
		return;
	}
	idx_t count = 0;
	const idx_t ncol = output.ColumnCount();
	while (state.offset < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = state.rows[state.offset];
		for (idx_t c = 0; c < ncol && c < row.size(); c++) {
			output.SetValue(c, count, row[c]);
		}
		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

TableFunction MakeFromXFunction(const string &name, const string &framework, const string &kind) {
	TableFunction tf(name, {LogicalType::VARCHAR}, FromXExec, FromXBind, FromXInit);
	tf.function_info = make_shared_ptr<FromXFunctionInfo>(framework, kind);
	return tf;
}

//===--------------------------------------------------------------------===//
// quack_from_x_sql(framework, kind) — expose embedded SQL for drift tests
//===--------------------------------------------------------------------===//

void FromXSqlScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t fw, string_t kind) {
		    auto *sql = quackapi_from_x_sql::Lookup(fw.GetString().c_str(), kind.GetString().c_str());
		    if (!sql) {
			    throw InvalidInputException("quack_from_x_sql: unknown framework/kind '%s'/'%s' "
			                                "(frameworks: fastapi|rails|express|gin; kind: routes|models)",
			                                fw.GetString(), kind.GetString());
		    }
		    return StringVector::AddString(result, sql);
	    });
}

//===--------------------------------------------------------------------===//
// quack_from_x_sql_relpath(framework, kind) — disk path for drift tests
//===--------------------------------------------------------------------===//

void FromXSqlRelpathScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t fw, string_t kind) {
		    for (const auto &e : quackapi_from_x_sql::All()) {
			    if (std::string(e.framework) == fw.GetString() && std::string(e.kind) == kind.GetString()) {
				    return StringVector::AddString(result, e.relpath);
			    }
		    }
		    throw InvalidInputException("quack_from_x_sql_relpath: unknown '%s'/'%s'", fw.GetString(),
		                                kind.GetString());
	    });
}

} // namespace

void RegisterQuackapiFromXFunctions(ExtensionLoader &loader) {
	// Route extractors
	loader.RegisterFunction(MakeFromXFunction("quack_from_fastapi", "fastapi", "routes"));
	loader.RegisterFunction(MakeFromXFunction("quack_from_rails", "rails", "routes"));
	loader.RegisterFunction(MakeFromXFunction("quack_from_express", "express", "routes"));
	loader.RegisterFunction(MakeFromXFunction("quack_from_gin", "gin", "routes"));

	// Model extractors
	loader.RegisterFunction(MakeFromXFunction("quack_from_fastapi_models", "fastapi", "models"));
	loader.RegisterFunction(MakeFromXFunction("quack_from_rails_models", "rails", "models"));
	loader.RegisterFunction(MakeFromXFunction("quack_from_express_models", "express", "models"));
	loader.RegisterFunction(MakeFromXFunction("quack_from_gin_models", "gin", "models"));

	// Embed drift helpers
	loader.RegisterFunction(ScalarFunction("quack_from_x_sql", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                       LogicalType::VARCHAR, FromXSqlScalar));
	loader.RegisterFunction(ScalarFunction("quack_from_x_sql_relpath", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                       LogicalType::VARCHAR, FromXSqlRelpathScalar));
}

} // namespace duckdb
