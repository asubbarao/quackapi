//===----------------------------------------------------------------------===//
// quackapi_post_provider.hpp
//
// HTTPUtil-backed outbound POST provider. Each executor worker owns its
// provider client state; clients are never shared across workers.
//===----------------------------------------------------------------------===//
#pragma once

#include "quackapi_post_result.hpp"

namespace duckdb {

struct QuackapiPostWorkerState;
struct QuackapiPostRequest;

struct QuackapiPostProvider {
	static unique_ptr<QuackapiPostWorkerState> CreateWorkerState(DatabaseInstance &db);
	static QuackapiHttpFetchResult Post(DatabaseInstance &db, QuackapiPostWorkerState &state,
	                                    const QuackapiPostRequest &request);
};

} // namespace duckdb
