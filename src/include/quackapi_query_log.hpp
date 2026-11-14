#pragma once
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/common/constants.hpp"
namespace duckdb {
class Connection;
struct QuackapiSqlSubmissionState : public ClientContextState {
	idx_t id = DConstants::INVALID_INDEX;
	idx_t parent_query_id = DConstants::INVALID_INDEX;
	idx_t parent_connection_id = DConstants::INVALID_INDEX;
	string origin;
	void Reset() {
		id = DConstants::INVALID_INDEX;
		parent_query_id = DConstants::INVALID_INDEX;
		parent_connection_id = DConstants::INVALID_INDEX;
		origin.clear();
	}
};
void QuackapiBeginSqlSubmission(Connection &con, const string &origin,
                                idx_t parent_query_id = DConstants::INVALID_INDEX,
                                idx_t parent_connection_id = DConstants::INVALID_INDEX);
} // namespace duckdb
