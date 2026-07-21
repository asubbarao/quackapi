#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! Register quack_from_{fastapi,rails,express,gin}[+_models] table functions
//! and quack_from_x_sql(framework, kind) for embed-drift tests.
void RegisterQuackapiFromXFunctions(ExtensionLoader &loader);

} // namespace duckdb
