// Integration regression: the last in-process request must not retain its DB.
#include "duckdb.hpp"

#include <cassert>
#include <stdexcept>

int main() {
	duckdb::weak_ptr<duckdb::DatabaseInstance> weak_database;
	{
		duckdb::DuckDB database(nullptr);
		weak_database = database.instance;
		duckdb::Connection connection(database);
		for (const auto &sql : {"LOAD quackapi", "CREATE ROUTE lifetime GET '/lifetime' AS SELECT 42 AS answer",
		                        "SELECT * FROM quackapi_request('GET', '/lifetime')"}) {
			auto result = connection.Query(sql);
			if (result->HasError()) {
				throw std::runtime_error(result->GetError());
			}
		}
	}
	// A TLS-owned request connection used to keep this reference alive, and
	// could destroy the DB after its allocator TLS during process shutdown.
	assert(weak_database.expired());
}
