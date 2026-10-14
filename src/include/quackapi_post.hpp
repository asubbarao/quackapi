//===----------------------------------------------------------------------===//
// quackapi_post.hpp
//
// SQL registration boundary for quackapi_post.
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

void RegisterQuackapiPostFunctions(ExtensionLoader &loader);

} // namespace duckdb
