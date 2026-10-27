#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! Register quack_from_routes(framework, path) and quack_from_models(framework, path).
void RegisterQuackapiFromXFunctions(ExtensionLoader &loader);

} // namespace duckdb
