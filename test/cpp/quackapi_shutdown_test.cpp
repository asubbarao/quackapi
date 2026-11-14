#include "duckdb.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
static std::mutex barrier_mutex;
static std::condition_variable barrier;
static bool entered = false, release_work = false;
static void Blocking(duckdb_function_info, duckdb_data_chunk input, duckdb_vector output) {
	std::unique_lock<std::mutex> guard(barrier_mutex);
	entered = true;
	barrier.notify_all();
	barrier.wait(guard, [] { return release_work; });
	auto values = static_cast<int32_t *>(duckdb_vector_get_data(output));
	for (idx_t i = 0; i < duckdb_data_chunk_get_size(input); i++)
		values[i] = 1;
}
static void Run(duckdb_connection con, const std::string &sql) {
	duckdb_result result;
	auto state = duckdb_query(con, sql.c_str(), &result);
	std::string error = state == DuckDBError ? duckdb_result_error(&result) : "";
	duckdb_destroy_result(&result);
	if (state == DuckDBError)
		throw std::runtime_error(error);
}
static void ExpectShutdownDenied(duckdb_connection con, const std::string &sql) {
	duckdb_result result;
	auto state = duckdb_query(con, sql.c_str(), &result);
	std::string error = state == DuckDBError ? duckdb_result_error(&result) : "";
	duckdb_destroy_result(&result);
	if (state != DuckDBError || error.find("shutting down") == std::string::npos)
		throw std::runtime_error("shutdown did not reject new admission: " + error);
}
int main(int argc, char **argv) {
	if (argc != 2)
		return 2;
	duckdb_database db = nullptr;
	duckdb_connection control = nullptr;
	try {
		duckdb_config config;
		duckdb_create_config(&config);
		duckdb_set_config(config, "allow_unsigned_extensions", "true");
		char *open_error = nullptr;
		auto opened = duckdb_open_ext(nullptr, &db, config, &open_error);
		duckdb_destroy_config(&config);
		if (opened == DuckDBError) {
			std::string error = open_error ? open_error : "open failed";
			duckdb_free(open_error);
			throw std::runtime_error(error);
		}
		duckdb_connect(db, &control);
		std::string candidate;
		for (char ch : std::string(argv[1])) {
			candidate += ch;
			if (ch == 39)
				candidate += ch;
		}
		Run(control, "LOAD '" + candidate + "'");
		Run(control, "CREATE ROUTE guard GET '/shutdown-guard' AS SELECT * FROM quackapi_stop(wait:=true)");
		Run(control, "SELECT CASE WHEN NOT contains(decode(body),'Blocking shutdown must run outside') THEN "
		             "error(decode(body)) END FROM quackapi_request('GET','/shutdown-guard')");
		Run(control, "SELECT CASE WHEN quackapi_selfdispatch('FROM quackapi_stop(wait:=true)').status<>422 THEN "
		             "error('nested blocking stop was admitted') END");
		auto function = duckdb_create_scalar_function();
		duckdb_scalar_function_set_name(function, "shutdown_probe");
		auto result_type = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
		duckdb_scalar_function_set_return_type(function, result_type);
		duckdb_destroy_logical_type(&result_type);
		duckdb_scalar_function_set_volatile(function);
		duckdb_scalar_function_set_function(function, Blocking);
		auto registered = duckdb_register_scalar_function(control, function);
		duckdb_destroy_scalar_function(&function);
		if (registered == DuckDBError)
			throw std::runtime_error("register probe failed");
		std::exception_ptr worker_error;
		std::thread worker([&] {
			duckdb_connection work;
			duckdb_connect(db, &work);
			try {
				Run(work, "SELECT quackapi_selfdispatch('SELECT shutdown_probe() AS finished')");
			} catch (...) {
				worker_error = std::current_exception();
			}
			duckdb_disconnect(&work);
		});
		{
			std::unique_lock<std::mutex> guard(barrier_mutex);
			if (!barrier.wait_for(guard, std::chrono::seconds(5), [] { return entered; })) {
				release_work = true;
				barrier.notify_all();
				guard.unlock();
				worker.join();
				if (worker_error)
					std::rethrow_exception(worker_error);
				throw std::runtime_error("native work never entered");
			}
		}
		std::atomic<bool> returned {false}, premature {false};
		std::thread release([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			premature = returned.load();
			std::lock_guard<std::mutex> guard(barrier_mutex);
			release_work = true;
			barrier.notify_all();
		});
		std::exception_ptr shutdown_error;
		try {
			Run(control, "FROM quackapi_stop(wait:=true)");
		} catch (...) {
			shutdown_error = std::current_exception();
		}
		returned = true;
		release.join();
		worker.join();
		if (shutdown_error)
			std::rethrow_exception(shutdown_error);
		if (worker_error)
			std::rethrow_exception(worker_error);
		if (premature)
			throw std::runtime_error("shutdown returned before native work finished");
		ExpectShutdownDenied(control, "SELECT quackapi_selfdispatch('SELECT 1')");
		ExpectShutdownDenied(control, "FROM quackapi_serve(9495)");
		duckdb_disconnect(&control);
		duckdb_close(&db);
		std::cout << "PASS: blocking shutdown waits, nested calls reject, admission stays closed" << std::endl;
		return 0;
	} catch (const std::exception &error) {
		if (control)
			duckdb_disconnect(&control);
		if (db)
			duckdb_close(&db);
		std::cerr << error.what() << std::endl;
		return 1;
	}
}
