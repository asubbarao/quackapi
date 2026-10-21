// Integration regression: interrupting a query (Ctrl-C) that is waiting in
// quackapi_exec. No deadline is near (the budget is 20 s), so neither the call's
// budget nor a request deadline can be what ends it.
//
// 1. The caller returns at once, and the interrupt reaches the inner query: the
//    inner connection releases the database long before its budget.
// 2. The caller returns at once even while a row is blocked where no interrupt
//    reaches (an outbound call to a peer that answers after 3 s).
#include "duckdb.hpp"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using clock_type = std::chrono::steady_clock;

long long MillisSince(clock_type::time_point start) {
	return static_cast<long long>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - start).count());
}

void Run(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	if (result->HasError()) {
		throw std::runtime_error(sql + ": " + result->GetError());
	}
}

//! Runs `sql` on `connection`, interrupts it after 300 ms, and returns how long the caller then took.
long long InterruptAfter300Ms(duckdb::Connection &connection, const std::string &sql) {
	duckdb::unique_ptr<duckdb::QueryResult> result;
	std::thread caller([&]() { result = connection.Query(sql); });
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	const auto interrupted_at = clock_type::now();
	connection.Interrupt();
	caller.join();
	const auto caller_ms = MillisSince(interrupted_at);
	if (!result->HasError() || result->GetError().find("Interrupt") == std::string::npos) {
		throw std::runtime_error("expected an interrupted query, got: " +
		                         (result->HasError() ? result->GetError() : std::string("a result")));
	}
	return caller_ms;
}

//! Waits until every connection to the database is gone; returns how long that took.
long long WaitForRelease(const duckdb::weak_ptr<duckdb::DatabaseInstance> &weak_database, long long limit_ms) {
	const auto start = clock_type::now();
	while (!weak_database.expired()) {
		if (MillisSince(start) > limit_ms) {
			return -1;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return MillisSince(start);
}
} // namespace

int main() {
	{
		duckdb::weak_ptr<duckdb::DatabaseInstance> weak_database;
		long long caller_ms = 0;
		{
			duckdb::DuckDB database(nullptr);
			weak_database = database.instance;
			duckdb::Connection connection(database);
			Run(connection, "LOAD quackapi");
			Run(connection, "SET quackapi_query_timeout_ms = 20000");
			caller_ms =
			    InterruptAfter300Ms(connection, "SELECT quackapi_exec('SELECT sum(range) FROM range(100000000000)')");
		}
		printf("1. caller returned %lld ms after the interrupt\n", caller_ms);
		if (caller_ms > 2000) {
			fprintf(stderr, "1. FAILED: the interrupted caller took %lld ms to return\n", caller_ms);
			return 1;
		}
		// The inner connection holds the database until its row ends; without the
		// interrupt it would run for its full 20 s budget.
		const auto released_ms = WaitForRelease(weak_database, 5000);
		if (released_ms < 0) {
			fprintf(stderr, "1. FAILED: the inner query was still running 5 s after the interrupt\n");
			return 1;
		}
		printf("1. inner query ended %lld ms after the caller\n", released_ms);
	}
	{
		duckdb::weak_ptr<duckdb::DatabaseInstance> weak_database;
		long long caller_ms = 0;
		{
			duckdb::DuckDB database(nullptr);
			weak_database = database.instance;
			duckdb::Connection connection(database);
			Run(connection, "LOAD quackapi");
			Run(connection, "SET quackapi_query_timeout_ms = 20000");
			Run(connection, "CREATE ROUTE slow GET '/slow' AS SELECT sleep_ms(3000) AS slept");
			Run(connection, "SELECT * FROM quackapi_serve(29340, host := '127.0.0.1', access_log := false)");
			caller_ms = InterruptAfter300Ms(
			    connection, "SELECT quackapi_exec('SELECT quackapi_fetch(''http://127.0.0.1:29340/slow'') AS f')");
			Run(connection, "SELECT * FROM quackapi_stop(29340)");
		}
		printf("2. caller returned %lld ms after the interrupt\n", caller_ms);
		if (caller_ms > 1000) {
			fprintf(stderr, "2. FAILED: the caller waited %lld ms for a row blocked in an outbound call\n", caller_ms);
			return 1;
		}
		// Let the blocked row finish before the process exits.
		if (WaitForRelease(weak_database, 10000) < 0) {
			fprintf(stderr, "2. FAILED: the blocked row never ended\n");
			return 1;
		}
	}
	return 0;
}
