#include "quackapi_openapi.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement.hpp"

#include "quackapi_state.hpp"

namespace duckdb {

namespace {

string JsonEscape(const string &input) {
	string result;
	result.reserve(input.size() + 2);
	for (unsigned char c : input) {
		switch (c) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\b':
			result += "\\b";
			break;
		case '\f':
			result += "\\f";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				result += buf;
			} else {
				result += static_cast<char>(c);
			}
		}
	}
	return result;
}

string JsonString(const string &s) {
	return "\"" + JsonEscape(s) + "\"";
}

vector<string> SplitPath(const string &path) {
	vector<string> segments;
	string current;
	for (char c : path) {
		if (c == '/') {
			if (!current.empty()) {
				segments.push_back(current);
				current.clear();
			}
		} else {
			current += c;
		}
	}
	if (!current.empty()) {
		segments.push_back(current);
	}
	return segments;
}

//! Extract path parameter names from :name / {name} segments.
vector<string> PathParamNames(const string &pattern) {
	vector<string> names;
	for (auto &ps : SplitPath(pattern)) {
		if (!ps.empty() && ps[0] == ':') {
			names.push_back(ps.substr(1));
		} else if (ps.size() >= 2 && ps.front() == '{' && ps.back() == '}') {
			names.push_back(ps.substr(1, ps.size() - 2));
		}
	}
	return names;
}

//! Convert /items/:id → /items/{id} for OpenAPI paths.
string OasPath(const string &pattern) {
	auto segments = SplitPath(pattern);
	string out;
	if (pattern.empty() || pattern[0] != '/') {
		out = "/";
	}
	for (auto &ps : segments) {
		out += "/";
		if (!ps.empty() && ps[0] == ':') {
			out += "{" + ps.substr(1) + "}";
		} else if (ps.size() >= 2 && ps.front() == '{' && ps.back() == '}') {
			out += ps; // already OAS form
		} else {
			out += ps;
		}
	}
	if (out.empty()) {
		out = "/";
	}
	return out;
}

bool IsClaimsParamName(const string &param_name) {
	return StringUtil::StartsWith(StringUtil::Lower(param_name), "claims_") && param_name.size() > 7;
}

bool ListContains(const vector<string> &list, const string &name) {
	for (auto &s : list) {
		if (StringUtil::Lower(s) == StringUtil::Lower(name)) {
			return true;
		}
	}
	return false;
}

//! DuckDB LogicalType → OpenAPI 3.1 schema fragment (JSON object body).
string DuckTypeToOas(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return "{\"type\":\"boolean\"}";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		return "{\"type\":\"integer\"}";
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
		return "{\"type\":\"integer\",\"format\":\"int64\"}";
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return "{\"type\":\"number\",\"format\":\"double\"}";
	case LogicalTypeId::DECIMAL:
		return "{\"type\":\"number\"}";
	case LogicalTypeId::DATE:
		return "{\"type\":\"string\",\"format\":\"date\"}";
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_TZ:
		return "{\"type\":\"string\",\"format\":\"time\"}";
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return "{\"type\":\"string\",\"format\":\"date-time\"}";
	case LogicalTypeId::LIST:
		return "{\"type\":\"array\",\"items\":{}}";
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::MAP:
	case LogicalTypeId::UNION:
		return "{\"type\":\"object\"}";
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::UUID:
	case LogicalTypeId::BIT:
	default: {
		// JSON extension type and other aliases fall through as string/object-ish.
		auto name = StringUtil::Lower(type.ToString());
		if (name == "json" || StringUtil::StartsWith(name, "struct") || StringUtil::StartsWith(name, "map")) {
			return "{\"type\":\"object\"}";
		}
		return "{\"type\":\"string\"}";
	}
	}
}

string ParamTypeToOas(const LogicalType &type) {
	// Path/query params are always strings on the wire; surface concrete types when known.
	if (type.id() == LogicalTypeId::UNKNOWN || type.id() == LogicalTypeId::VARCHAR) {
		return "{\"type\":\"string\"}";
	}
	return DuckTypeToOas(type);
}

enum class ResponseMode { JSON, HTML, TEXT };

ResponseMode ModeFor(const vector<string> &names) {
	if (names.size() != 1) {
		return ResponseMode::JSON;
	}
	auto lower = StringUtil::Lower(names[0]);
	if (lower == "html") {
		return ResponseMode::HTML;
	}
	if (lower == "text") {
		return ResponseMode::TEXT;
	}
	return ResponseMode::JSON;
}

string AuthKindString(QuackapiAuthKind kind) {
	switch (kind) {
	case QuackapiAuthKind::API_KEY:
		return "API_KEY";
	case QuackapiAuthKind::JWT_HS256:
		return "JWT_HS256";
	}
	return "API_KEY";
}

} // namespace

string BuildOpenApiDocument(DatabaseInstance &db, const string &server_url) {
	auto routes = QuackapiState::Get(db).SnapshotRoutes();
	auto auths = QuackapiState::Get(db).SnapshotAuths();

	// path → method_lc → operation JSON object
	// Use ordered vectors for stable output.
	struct OpEntry {
		string oas_path;
		string method_lc;
		string operation_json;
	};
	vector<OpEntry> ops;

	Connection con(db);

	for (auto &route : routes) {
		vector<string> col_names;
		vector<LogicalType> col_types;
		case_insensitive_map_t<LogicalType> expected_types;
		vector<string> named_params;

		auto prepared = con.Prepare(route.handler_sql);
		if (!prepared->HasError()) {
			col_names = prepared->GetNames();
			col_types = prepared->GetTypes();
			expected_types = prepared->GetExpectedParameterTypes();
			for (auto &entry : prepared->named_param_map) {
				named_params.push_back(entry.first);
			}
		}

		auto path_params = PathParamNames(route.pattern);
		auto oas_path = OasPath(route.pattern);
		auto mode = ModeFor(col_names);

		// parameters: path + query
		string params_json = "[";
		bool first_param = true;
		for (auto &n : path_params) {
			if (!first_param) {
				params_json += ",";
			}
			first_param = false;
			LogicalType ptype = LogicalType::VARCHAR;
			auto tit = expected_types.find(n);
			if (tit != expected_types.end()) {
				ptype = tit->second;
			}
			params_json += "{\"name\":" + JsonString(n) + ",\"in\":\"path\",\"required\":true,\"schema\":" +
			               ParamTypeToOas(ptype) + "}";
		}
		for (auto &n : named_params) {
			if (IsClaimsParamName(n) || ListContains(path_params, n)) {
				continue;
			}
			if (!first_param) {
				params_json += ",";
			}
			first_param = false;
			LogicalType ptype = LogicalType::VARCHAR;
			auto tit = expected_types.find(n);
			if (tit != expected_types.end()) {
				ptype = tit->second;
			}
			params_json += "{\"name\":" + JsonString(n) + ",\"in\":\"query\",\"required\":true,\"schema\":" +
			               ParamTypeToOas(ptype) + "}";
		}
		params_json += "]";

		// response schema
		string response_schema;
		string content_type;
		string response_desc;
		if (mode == ResponseMode::HTML) {
			content_type = "text/html";
			response_desc = "HTML response";
			response_schema =
			    "{\"type\":\"string\",\"description\":\"Raw HTML body (single-column handler AS html)\"}";
		} else if (mode == ResponseMode::TEXT) {
			content_type = "text/plain";
			response_desc = "Plain text response";
			response_schema =
			    "{\"type\":\"string\",\"description\":\"Raw text body (single-column handler AS text)\"}";
		} else {
			content_type = "application/json";
			response_desc = "Successful response (JSON array of row objects)";
			string props = "{";
			for (idx_t i = 0; i < col_names.size(); i++) {
				if (i > 0) {
					props += ",";
				}
				LogicalType t = i < col_types.size() ? col_types[i] : LogicalType::VARCHAR;
				props += JsonString(col_names[i]) + ":" + DuckTypeToOas(t);
			}
			props += "}";
			response_schema = "{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":" + props + "}}";
		}

		string status_key = std::to_string(route.status);
		string responses =
		    "{" + JsonString(status_key) + ":{"
		                                   "\"description\":" +
		    JsonString(response_desc) + ","
		                                "\"content\":{" +
		    JsonString(content_type) + ":{\"schema\":" + response_schema + "}}"
		                                                                   "}," +
		    "\"422\":{"
		    "\"description\":\"Validation error (FastAPI-shaped detail)\","
		    "\"content\":{\"application/json\":{\"schema\":{"
		    "\"type\":\"object\","
		    "\"properties\":{\"detail\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}}"
		    "}}}}"
		    "}";

		string security = "[]";
		if (!route.require_auth.empty()) {
			security = "[{" + JsonString(route.require_auth) + ":[]}]";
		}

		string method_lc = StringUtil::Lower(route.method);
		string op = "{";
		op += "\"operationId\":" + JsonString(route.name) + ",";
		op += "\"summary\":" + JsonString(route.name) + ",";
		op += "\"parameters\":" + params_json + ",";
		op += "\"responses\":" + responses + ",";
		op += "\"security\":" + security;
		op += "}";

		ops.push_back({oas_path, method_lc, op});
	}

	// Group into path items
	// Collect unique paths in first-seen order
	vector<string> path_order;
	for (auto &op : ops) {
		if (!ListContains(path_order, op.oas_path)) {
			path_order.push_back(op.oas_path);
		}
	}

	string paths = "{";
	for (idx_t pi = 0; pi < path_order.size(); pi++) {
		if (pi > 0) {
			paths += ",";
		}
		auto &p = path_order[pi];
		paths += JsonString(p) + ":{";
		bool first_m = true;
		for (auto &op : ops) {
			if (op.oas_path != p) {
				continue;
			}
			if (!first_m) {
				paths += ",";
			}
			first_m = false;
			paths += JsonString(op.method_lc) + ":" + op.operation_json;
		}
		paths += "}";
	}
	paths += "}";

	// securitySchemes
	string schemes = "{";
	for (idx_t i = 0; i < auths.size(); i++) {
		if (i > 0) {
			schemes += ",";
		}
		auto &a = auths[i];
		schemes += JsonString(a.name) + ":";
		if (a.kind == QuackapiAuthKind::JWT_HS256) {
			schemes += "{\"type\":\"http\",\"scheme\":\"bearer\",\"bearerFormat\":\"JWT\"}";
		} else {
			schemes += "{\"type\":\"apiKey\",\"in\":\"header\",\"name\":" + JsonString(a.header) + "}";
		}
		(void)AuthKindString;
	}
	schemes += "}";

	string doc = "{";
	doc += "\"openapi\":\"3.1.0\",";
	doc += "\"info\":{";
	doc += "\"title\":\"quackapi\",";
	doc += "\"version\":\"0.1.0\",";
	doc += "\"description\":" +
	       JsonString("Generated from the live quackapi_routes() + quackapi_auths() registries. "
	                  "Response schemas come from Prepare() column metadata. Path params :id → {id}. "
	                  "require_auth maps to security schemes.") +
	       "},";
	doc += "\"servers\":[{\"url\":" + JsonString(server_url) + ",\"description\":\"quackapi_serve\"}],";
	doc += "\"paths\":" + paths + ",";
	doc += "\"components\":{\"securitySchemes\":" + schemes + "}";
	doc += "}";
	return doc;
}

string OpenApiDocsHtml() {
	// Swagger UI 5.x from unpkg CDN, pointing at /openapi.json (same origin).
	return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>quackapi — interactive API docs</title>
  <link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5.17.14/swagger-ui.css" />
  <style>
    html { box-sizing: border-box; overflow-y: scroll; }
    *, *::before, *::after { box-sizing: inherit; }
    body { margin: 0; background: #fafafa; font-family: system-ui, sans-serif; }
    .banner {
      background: #1b1b1b; color: #f0f0f0; padding: 0.75rem 1.25rem;
      font-size: 0.9rem; border-bottom: 3px solid #00c853;
    }
    .banner code { background: #333; padding: 0.1rem 0.35rem; border-radius: 3px; }
    .banner a { color: #69f0ae; }
  </style>
</head>
<body>
  <div class="banner">
    <strong>quackapi</strong> — live OpenAPI from <code>/openapi.json</code>
    (route registry + Prepare() types). Built-in; not listed in <code>quackapi_routes()</code>.
  </div>
  <div id="swagger-ui"></div>
  <script src="https://unpkg.com/swagger-ui-dist@5.17.14/swagger-ui-bundle.js" crossorigin></script>
  <script src="https://unpkg.com/swagger-ui-dist@5.17.14/swagger-ui-standalone-preset.js" crossorigin></script>
  <script>
    window.onload = function () {
      window.ui = SwaggerUIBundle({
        url: "/openapi.json",
        dom_id: "#swagger-ui",
        deepLinking: true,
        presets: [SwaggerUIBundle.presets.apis, SwaggerUIStandalonePreset],
        plugins: [SwaggerUIBundle.plugins.DownloadUrl],
        layout: "StandaloneLayout",
        tryItOutEnabled: true,
        supportedSubmitMethods: ["get", "post", "put", "delete", "patch", "head"]
      });
    };
  </script>
</body>
</html>
)HTML";
}

string OpenApiRedocHtml() {
	return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <title>quackapi — ReDoc</title>
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <style>body { margin: 0; padding: 0; }</style>
</head>
<body>
  <redoc spec-url="/openapi.json"></redoc>
  <script src="https://cdn.redoc.ly/redoc/latest/bundles/redoc.standalone.js"></script>
</body>
</html>
)HTML";
}

} // namespace duckdb
