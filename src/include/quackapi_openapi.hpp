#pragma once

#include "duckdb/common/string.hpp"

namespace duckdb {

class DatabaseInstance;

//! Build an OpenAPI 3.1 document from the live route + auth registries.
//! Response column names/types come from Prepare() metadata (no execute).
//! Path params :id/{id} → {id}; query params = handler $params minus path/claims.
//! require_auth → security; CREATE AUTH kinds → components.securitySchemes.
//! Built-in — not registered in quackapi_routes().
string BuildOpenApiDocument(DatabaseInstance &db, const string &server_url = "http://127.0.0.1:8000");

//! Swagger UI HTML shell that loads /openapi.json (CDN assets).
string OpenApiDocsHtml();

//! Optional Redoc shell at /redoc.
string OpenApiRedocHtml();

} // namespace duckdb
