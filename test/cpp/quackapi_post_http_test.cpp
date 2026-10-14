// Real HTTP regression checks for row-local POST, side effects, and the
// database-wide concurrency limit. Arguments are explicit artifact paths and
// their expected extension versions; this must never load an installed copy.
#include "duckdb.hpp"
#include "httplib.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace duckdb;

static void Check(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

static std::unique_ptr<MaterializedQueryResult> Run(Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Check(!result->HasError(), sql + ": " + result->GetError());
	return result;
}

static std::string SqlQuote(const std::string &value) {
	Check(value.find('\'') == std::string::npos, "artifact path contains a SQL quote");
	return "'" + value + "'";
}

static void CheckExtension(Connection &connection, const std::string &name, const std::string &expected_version) {
	auto result = Run(connection, "SELECT extension_version, loaded FROM duckdb_extensions() WHERE extension_name = " +
	                                  SqlQuote(name));
	Check(result->RowCount() == 1, name + " extension metadata missing");
	Check(result->GetValue(0, 0).ToString() == expected_version, name + " artifact version mismatch");
	Check(result->GetValue(1, 0).GetValue<bool>(), name + " extension is not loaded");
}

struct ListenerGuard {
	duckdb_httplib::Server &server;
	std::thread &listener;
	~ListenerGuard() {
		server.stop();
		if (listener.joinable()) {
			listener.join();
		}
	}
};

int main(int argc, char **argv) {
	try {
		Check(argc == 5,
		      "usage: quackapi_post_http_test QUACKAPI_PATH QUACKAPI_VERSION PROVIDER_PATH PROVIDER_VERSION");
		std::atomic<int> received {0};
		std::atomic<int> active {0};
		std::atomic<int> peak {0};
		duckdb_httplib::Server server;
		server.new_task_queue = []() {
			return new duckdb_httplib::ThreadPool(48);
		};
		server.Post("/echo", [&](const duckdb_httplib::Request &request, duckdb_httplib::Response &response) {
			received.fetch_add(1);
			response.set_header("X-Echo-Content-Type", request.get_header_value("Content-Type"));
			response.set_content(request.body, "text/plain");
		});
		server.Post("/headers", [&](const duckdb_httplib::Request &request, duckdb_httplib::Response &response) {
			received.fetch_add(1);
			const auto content_types = request.headers.equal_range("Content-Type");
			response.set_content(request.get_header_value("Content-Type") + "|" + request.get_header_value("X-Trace") +
			                         "|" + std::to_string(std::distance(content_types.first, content_types.second)),
			                     "text/plain");
		});
		server.Post("/slow", [&](const duckdb_httplib::Request &request, duckdb_httplib::Response &response) {
			received.fetch_add(1);
			const auto now = active.fetch_add(1) + 1;
			int seen = peak.load();
			while (now > seen && !peak.compare_exchange_weak(seen, now)) {
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(request.body == "wide" ? 900 : 180));
			active.fetch_sub(1);
			response.set_content(request.body, "text/plain");
		});
		const int port = server.bind_to_any_port("127.0.0.1");
		Check(port > 0, "could not bind loopback HTTP fixture");
		std::thread listener([&]() { server.listen_after_bind(); });
		ListenerGuard guard {server, listener};
		server.wait_until_ready();
		duckdb_httplib::Client probe("127.0.0.1", port);
		auto probe_response = probe.Post("/echo", "probe", "text/plain");
		Check(probe_response && probe_response->status == 200 && probe_response->body == "probe",
		      "loopback fixture did not serve its own POST probe");
		received.store(0);

		DBConfig config;
		config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
		DuckDB database(nullptr, &config);
		Connection connection(database);
		auto version = Run(connection, "PRAGMA version");
		Check(version->GetValue(0, 0).ToString() == "v1.5.6" && version->GetValue(1, 0).ToString() == "069cc9f9",
		      "test core must be DuckDB 069cc9f9 v1.5.6");
		Run(connection, "LOAD " + SqlQuote(argv[3]));
		CheckExtension(connection, "curl_httpfs", argv[4]);
		Run(connection, "LOAD " + SqlQuote(argv[1]));
		CheckExtension(connection, "quackapi", argv[2]);
		const std::string echo_url = "http://127.0.0.1:" + std::to_string(port) + "/echo";
		const std::string headers_url = "http://127.0.0.1:" + std::to_string(port) + "/headers";
		const std::string slow_url = "http://127.0.0.1:" + std::to_string(port) + "/slow";
		auto headers = Run(connection, "SELECT receipt.status, receipt.body FROM "
		                               "(SELECT quackapi_post(" +
		                                   SqlQuote(headers_url) +
		                                   ", '{}', 'application/vnd.quackapi-test+json', "
		                                   "MAP {'X-Trace': 'trace-42'}) AS receipt)");
		Check(headers->GetValue(0, 0).GetValue<int>() == 200 &&
		          headers->GetValue(1, 0).ToString() == "application/vnd.quackapi-test+json|trace-42|1",
		      "provider changed explicit Content-Type or custom headers");
		auto default_headers = Run(connection, "SELECT receipt.status, receipt.body FROM "
		                                       "(SELECT quackapi_post(" +
		                                           SqlQuote(headers_url) + ", '{}') AS receipt)");
		Check(default_headers->GetValue(0, 0).GetValue<int>() == 200 &&
		          default_headers->GetValue(1, 0).ToString() == "application/json||1",
		      "provider duplicated or changed default Content-Type");
		Check(received.load() == 2, "header probes made an unexpected number of requests");

		// Distinct input rows must each reach the wire and return their own body.
		auto rows =
		    Run(connection, "SELECT i, receipt.status, receipt.body, receipt.error FROM "
		                    "(SELECT i, quackapi_post(" +
		                        SqlQuote(echo_url) +
		                        ", printf('row:%d', i), 'text/plain') AS receipt FROM range(40) t(i)) ORDER BY i");
		Check(rows->RowCount() == 40, "POST collapsed distinct rows");
		for (idx_t i = 0; i < 40; i++) {
			Check(rows->GetValue(1, i).GetValue<int>() == 200,
			      "POST row failed at " + std::to_string(i) + ": " + rows->GetValue(3, i).ToString());
			Check(rows->GetValue(2, i).ToString() == "row:" + std::to_string(i), "POST body crossed rows");
		}
		Check(received.load() == 42, "distinct rows made the wrong number of HTTP requests");

		// A constant volatile call still sends once per row.
		auto constant = Run(connection, "SELECT (quackapi_post(" + SqlQuote(echo_url) +
		                                    ", 'same', 'text/plain')).status FROM range(16)");
		Check(constant->RowCount() == 16 && received.load() == 58, "constant POST was deduplicated");

		// One transport failure must not discard successful peers in the same vector.
		auto mixed =
		    Run(connection, "SELECT i, receipt.status, receipt.error FROM "
		                    "(SELECT i, quackapi_post(CASE WHEN i = 1 THEN 'http://127.0.0.1:1/' ELSE " +
		                        SqlQuote(echo_url) + " END, 'mixed') AS receipt FROM range(3) t(i)) ORDER BY i");
		Check(mixed->GetValue(1, 0).GetValue<int>() == 200 && mixed->GetValue(1, 2).GetValue<int>() == 200,
		      "a failed POST poisoned successful rows");
		Check(mixed->GetValue(1, 1).GetValue<int>() == 0 && !mixed->GetValue(2, 1).IsNull(),
		      "transport failure lost its row-local error");
		Check(received.load() == 60, "mixed batch retried or lost requests");

		auto concurrent = [&](int callers_count, const std::string &body) {
			std::atomic<int> ready {0};
			std::atomic<bool> go {false};
			std::atomic<int> passed {0};
			std::vector<std::thread> callers;
			for (int i = 0; i < callers_count; i++) {
				callers.emplace_back([&]() {
					Connection caller(database);
					ready.fetch_add(1);
					while (!go.load()) {
						std::this_thread::yield();
					}
					auto result = caller.Query("SELECT (quackapi_post(" + SqlQuote(slow_url) + ", " + SqlQuote(body) +
					                           ")).status");
					if (!result->HasError() && result->GetValue(0, 0).GetValue<int>() == 200) {
						passed.fetch_add(1);
					}
				});
			}
			while (ready.load() != callers_count) {
				std::this_thread::yield();
			}
			go.store(true);
			for (auto &caller : callers) {
				caller.join();
			}
			return passed.load();
		};

		Run(connection, "SET quackapi_post_concurrency = 4");
		Check(concurrent(10, "x") == 10, "concurrent POST lost a request");
		Check(peak.load() > 1 && peak.load() <= 4, "POST ignored database-wide concurrency limit");
		Check(received.load() == 70, "concurrent POST retried or lost requests");

		// A positive bound above 32 must be effective, not silently clamped.
		peak.store(0);
		Run(connection, "SET quackapi_post_concurrency = 40");
		Check(concurrent(36, "wide") == 36, "wide POST batch lost a request");
		Check(peak.load() > 32 && peak.load() <= 40, "POST concurrency was capped at 32");
		Check(received.load() == 106, "wide POST batch retried or lost requests");
		DBConfig other_config;
		other_config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
		DuckDB other_database(nullptr, &other_config);
		Connection other_setup(other_database);
		Run(other_setup, "LOAD " + SqlQuote(argv[3]));
		Run(other_setup, "LOAD " + SqlQuote(argv[1]));
		CheckExtension(other_setup, "curl_httpfs", argv[4]);
		CheckExtension(other_setup, "quackapi", argv[2]);
		Run(connection, "SET quackapi_post_concurrency = 1");
		Run(other_setup, "SET quackapi_post_concurrency = 1");
		peak.store(0);
		std::atomic<int> independent_ready {0};
		std::atomic<bool> independent_go {false};
		std::atomic<int> independent_passed {0};
		auto call_database = [&](DuckDB &target) {
			Connection caller(target);
			independent_ready.fetch_add(1);
			while (!independent_go.load()) {
				std::this_thread::yield();
			}
			auto result = caller.Query("SELECT (quackapi_post(" + SqlQuote(slow_url) + ", 'wide')).status");
			if (!result->HasError() && result->GetValue(0, 0).GetValue<int>() == 200) {
				independent_passed.fetch_add(1);
			}
		};
		std::thread first_database_call([&]() { call_database(database); });
		std::thread second_database_call([&]() { call_database(other_database); });
		while (independent_ready.load() != 2) {
			std::this_thread::yield();
		}
		independent_go.store(true);
		first_database_call.join();
		second_database_call.join();
		Check(independent_passed.load() == 2 && peak.load() == 2,
		      "independent databases shared one POST concurrency limit");
		Check(received.load() == 108, "independent databases lost or retried a request");

		// Interrupt a request waiting behind the sole active worker. It must
		// leave the queue promptly and never reach the HTTP fixture.
		Connection first(database);
		Connection queued(database);
		std::atomic<bool> first_ok {false};
		std::atomic<bool> queued_failed {false};
		std::thread first_call([&]() {
			auto result = first.Query("SELECT (quackapi_post(" + SqlQuote(slow_url) + ", 'wide')).status");
			first_ok.store(!result->HasError() && result->GetValue(0, 0).GetValue<int>() == 200);
		});
		while (active.load() == 0) {
			std::this_thread::yield();
		}
		const auto queued_at = std::chrono::steady_clock::now();
		std::thread queued_call([&]() {
			auto result = queued.Query("SELECT (quackapi_post(" + SqlQuote(slow_url) + ", 'queued')).status");
			queued_failed.store(result->HasError());
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		queued.Interrupt();
		queued_call.join();
		const auto queued_elapsed = std::chrono::steady_clock::now() - queued_at;
		first_call.join();
		Check(first_ok.load(), "active POST did not finish after peer cancellation");
		Check(queued_failed.load() && queued_elapsed < std::chrono::milliseconds(600),
		      "queued POST did not cancel promptly");
		Check(received.load() == 109, "cancelled queued POST reached the server");

		// The inbound route deadline must bound its outbound POST, even after
		// the provider has started receiving a slow response.
		Run(connection, "CREATE ROUTE post_deadline GET '/post-deadline' AS SELECT "
		                "(quackapi_post(" +
		                    SqlQuote(slow_url) + ", 'wide')).status AS upstream_status");
		const auto deadline_at = std::chrono::steady_clock::now();
		auto timed = Run(connection, "SELECT status FROM quackapi_request('GET', '/post-deadline', "
		                             "query_timeout_ms := 100)");
		const auto deadline_elapsed = std::chrono::steady_clock::now() - deadline_at;
		const auto deadline_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_elapsed).count();
		Check(timed->GetValue(0, 0).GetValue<int>() == 504 && deadline_elapsed < std::chrono::milliseconds(600),
		      "in-flight POST outlived its inbound route deadline: status=" + timed->GetValue(0, 0).ToString() +
		          " elapsed_ms=" + std::to_string(deadline_ms));
		while (active.load() != 0) {
			std::this_thread::yield();
		}
		Connection interrupted(database);
		std::atomic<bool> active_failed {false};
		std::thread active_call([&]() {
			auto result = interrupted.Query("SELECT (quackapi_post(" + SqlQuote(slow_url) + ", 'wide')).status");
			active_failed.store(result->HasError());
		});
		while (active.load() == 0) {
			std::this_thread::yield();
		}
		const auto active_at = std::chrono::steady_clock::now();
		interrupted.Interrupt();
		active_call.join();
		const auto active_elapsed = std::chrono::steady_clock::now() - active_at;
		const auto active_ms = std::chrono::duration_cast<std::chrono::milliseconds>(active_elapsed).count();
		Check(active_failed.load() && active_elapsed < std::chrono::milliseconds(600),
		      "active POST ignored caller interrupt: elapsed_ms=" + std::to_string(active_ms));
		std::cout << "POST HTTP integration passed: 111 requests, wide peak=" << peak.load() << '\n';
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
