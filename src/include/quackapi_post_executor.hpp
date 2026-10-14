//===----------------------------------------------------------------------===//
// quackapi_post_executor.hpp
//
// Scheduling boundary for one scalar-function input chunk. This phase keeps
// the existing per-chunk worker behavior; the boundary allows the following
// executor change without altering POST registration or provider code.
//===----------------------------------------------------------------------===//
#pragma once

#include "quackapi_http_fetch.hpp"

#include <functional>

namespace duckdb {

using QuackapiPostOperation = std::function<QuackapiHttpFetchResult(idx_t)>;

struct QuackapiPostExecutor {
	static void Execute(idx_t count, const vector<bool> &skip, vector<QuackapiHttpFetchResult> &results,
	                    const QuackapiPostOperation &perform);
};

} // namespace duckdb
