// Integration regression: interrupting a query (Ctrl-C) that is waiting in
// quackapi_exec. No deadline is near (the budget is 20 s), so neither the call's
// budget nor a request deadline can be what ends it.
//
// 1. The caller returns at once, and the interrupt reaches the inner query: the
//    inner connection releases the database long before its budget.
// 2. The caller returns at once even while a row is blocked where no interrupt
//    reaches (an outbound call to a peer that answers after 3 s).
// 3. A row whose caller was interrupted runs none of its later statements, though
//    DuckDB clears an interrupt as each statement starts.
// 4. Closing the database while an abandoned row is still blocked waits for that
//    row: when the close returns, the database is gone.
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

std::string Scalar(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	if (result->HasError()) {
		throw std::runtime_error(sql + ": " + result->GetError());
	}
	return result->Fetch()->GetValue(0, 0).ToString();
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
			Run(connection, "CREATE ROUTE slow POST '/slow' AS SELECT sleep_ms(3000) AS slept");
			Run(connection, "SELECT * FROM quackapi_serve(29340, host := '127.0.0.1', access_log := false)");
			caller_ms = InterruptAfter300Ms(
			    connection, "SELECT quackapi_exec('SELECT quackapi_post(''http://127.0.0.1:29340/slow'', '''') AS f')");
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
	{
		duckdb::weak_ptr<duckdb::DatabaseInstance> weak_database;
		long long caller_ms = 0;
		{
			duckdb::DuckDB database(nullptr);
			weak_database = database.instance;
			duckdb::Connection connection(database);
			Run(connection, "LOAD quackapi");
			Run(connection, "SET threads = 1");
			Run(connection, "SET async_threads = 2");
			Run(connection, "SET quackapi_query_timeout_ms = 20000");
			caller_ms = InterruptAfter300Ms(
			    connection, "SELECT * FROM quackapi_exec('SELECT sum(range) FROM range(100000000000)')");
		}
		printf("5. native table caller returned %lld ms after the interrupt\n", caller_ms);
		if (caller_ms > 2000 || WaitForRelease(weak_database, 5000) < 0) {
			fprintf(stderr, "5. FAILED: native table cancellation did not drain its submitted query\n");
			return 1;
		}
	}
	{
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		Run(connection, "LOAD quackapi");
		Run(connection, "SET threads = 4");
		Run(connection, "SET quackapi_query_timeout_ms = 2000");
		Run(connection, "SET max_execution_time = 100");
		for (const auto &sql : {"SELECT quackapi_exec('SELECT sum(range) FROM range(100000000000)')",
		                        "SELECT * FROM quackapi_exec('SELECT sum(range) FROM range(100000000000)')"}) {
			const auto start = clock_type::now();
			auto result = connection.Query(sql);
			const auto elapsed_ms = MillisSince(start);
			printf("6. native engine deadline returned in %lld ms\n", elapsed_ms);
			if (!result->HasError() || result->GetError().find("maximum execution time") == std::string::npos ||
			    elapsed_ms > 1000) {
				fprintf(stderr, "6. FAILED: nested execution did not inherit max_execution_time: %s\n",
				        result->HasError() ? result->GetError().c_str() : "successful result");
				return 1;
			}
		}
	}
	// Cases 3 and 4 both report before the test fails, so one run shows each.
	int failures = 0;
	{
		// 32 rows of 20000 short INSERTs, each stamped with its statement's start. A
		// statement that has passed its last interrupt check (its commit, its result)
		// finishes, and the next one clears the interrupt as it starts; without a check
		// between statements such a row runs on to its end.
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		Run(connection, "LOAD quackapi");
		Run(connection, "SET quackapi_query_timeout_ms = 20000");
		Run(connection, "CREATE TABLE ticks (r INTEGER, started TIMESTAMPTZ)");
		duckdb::unique_ptr<duckdb::QueryResult> result;
		std::thread caller([&]() {
			result = connection.Query("SELECT quackapi_exec(repeat('INSERT INTO ticks VALUES (' || range || ', "
			                          "get_current_timestamp());', 20000)) FROM range(32)");
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		const auto interrupted_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
		        .count();
		connection.Interrupt();
		caller.join();
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		// Allow 100 ms for a statement already under way when the interrupt landed.
		const auto late = Scalar(connection, "SELECT coalesce(list(DISTINCT r ORDER BY r)::VARCHAR, 'none') FROM ticks "
		                                     "WHERE epoch_us(started) > " +
		                                         std::to_string(interrupted_us + 100000));
		if (late != "none") {
			fprintf(stderr, "3. FAILED: rows %s went on starting statements after the interrupt\n", late.c_str());
			failures++;
		} else {
			printf("3. every row stopped within one statement of the interrupt\n");
		}
	}
	// A peer on a database of its own, answering after 1.5 s, outlives the caller below.
	duckdb::DuckDB peer_database(nullptr);
	duckdb::Connection peer(peer_database);
	Run(peer, "LOAD quackapi");
	Run(peer, "CREATE ROUTE slow POST '/slow' AS SELECT sleep_ms(1500) AS slept");
	Run(peer, "SELECT * FROM quackapi_serve(29341, host := '127.0.0.1', access_log := false, worker_threads := 8)");
	const std::string blocked_row = "SELECT quackapi_post(''http://127.0.0.1:29341/slow'', '''') AS f";
	{
		const std::string path = "quackapi_exec_close_test.duckdb";
		std::remove(path.c_str());
		std::remove((path + ".wal").c_str());
		duckdb::weak_ptr<duckdb::DatabaseInstance> weak_database;
		const auto opened_at = clock_type::now();
		{
			duckdb::DuckDB database(path);
			weak_database = database.instance;
			duckdb::Connection connection(database);
			Run(connection, "LOAD quackapi");
			Run(connection, "SET quackapi_query_timeout_ms = 20000");
			InterruptAfter300Ms(connection, "SELECT quackapi_exec('" + blocked_row + "')");
		}
		const auto closed_ms = MillisSince(opened_at);
		if (!weak_database.expired()) {
			fprintf(stderr, "4. FAILED: the database was still open when its close returned, %lld ms after opening\n",
			        closed_ms);
			// Let the blocked row finish before the process exits.
			if (WaitForRelease(weak_database, 10000) < 0) {
				fprintf(stderr, "4. and the blocked row never ended\n");
			}
			failures++;
		} else {
			printf("4. the database was gone when its close returned, %lld ms after opening\n", closed_ms);
		}
		std::remove(path.c_str());
		std::remove((path + ".wal").c_str());
	}
	Run(peer, "SELECT * FROM quackapi_stop(29341)");
	return failures == 0 ? 0 : 1;
}
