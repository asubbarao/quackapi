// Integration regression: interrupting a query (Ctrl-C) that is waiting in
// quackapi_exec returns at once and interrupts the inner query too. No deadline
// is near, so neither the call's budget nor a request deadline can be what ends it.
#include "duckdb.hpp"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

int main() {
	using clock = std::chrono::steady_clock;
	duckdb::weak_ptr<duckdb::DatabaseInstance> weak_database;
	{
		duckdb::DuckDB database(nullptr);
		weak_database = database.instance;
		duckdb::Connection connection(database);
		for (const auto &sql : {"LOAD quackapi", "SET quackapi_query_timeout_ms = 120000"}) {
			auto result = connection.Query(sql);
			if (result->HasError()) {
				throw std::runtime_error(result->GetError());
			}
		}
		duckdb::unique_ptr<duckdb::MaterializedQueryResult> result;
		std::thread caller([&]() {
			result = connection.Query("SELECT quackapi_exec('SELECT sum(range) FROM range(100000000000)')");
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		const auto interrupted_at = clock::now();
		connection.Interrupt();
		caller.join();
		const auto caller_ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - interrupted_at).count();
		if (!result->HasError() || result->GetError().find("Interrupt") == std::string::npos) {
			fprintf(stderr, "expected an interrupted query, got: %s\n",
			        result->HasError() ? result->GetError().c_str() : "a result");
			return 1;
		}
		if (caller_ms > 2000) {
			fprintf(stderr, "the interrupted caller took %lld ms to return\n", static_cast<long long>(caller_ms));
			return 1;
		}
		printf("caller returned %lld ms after the interrupt\n", static_cast<long long>(caller_ms));
	}
	// The inner query's connection holds the database until its row ends. If the
	// interrupt did not reach it, it would run for its full 120 s budget.
	const auto closed_at = clock::now();
	while (!weak_database.expired()) {
		if (clock::now() - closed_at > std::chrono::seconds(5)) {
			fprintf(stderr, "the inner query was still running 5 s after the interrupt\n");
			return 1;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	printf("inner query ended %lld ms after the caller\n",
	       static_cast<long long>(
	           std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - closed_at).count()));
	return 0;
}
