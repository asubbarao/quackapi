// A live POST retains its original HTTPUtil while another provider replaces
// the database's active implementation. Later POSTs must use a fresh client.
#include "duckdb.hpp"
#include "duckdb/common/http_util.hpp"
#include "httplib.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

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

static std::string Quote(const std::string &value) {
	Check(value.find('\'') == std::string::npos, "artifact path contains SQL quote");
	return "'" + value + "'";
}

int main(int argc, char **argv) {
	try {
		Check(argc == 7, "usage: provider_replacement QUACKAPI_PATH QUACKAPI_VERSION "
		                 "CURL_HTTPFS_PATH CURL_HTTPFS_VERSION HTTPFS_PATH HTTPFS_VERSION");
		std::atomic<int> received {0};
		std::atomic<bool> slow_active {false};
		duckdb_httplib::Server server;
		server.Post("/echo", [&](const duckdb_httplib::Request &request, duckdb_httplib::Response &response) {
			received.fetch_add(1);
			response.set_content(request.body, "text/plain");
		});
		server.Post("/slow", [&](const duckdb_httplib::Request &request, duckdb_httplib::Response &response) {
			received.fetch_add(1);
			slow_active.store(true);
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			response.set_content(request.body, "text/plain");
		});
		const int port = server.bind_to_any_port("127.0.0.1");
		Check(port > 0, "could not bind replacement fixture");
		std::thread listener([&]() { server.listen_after_bind(); });
		struct ListenerGuard {
			duckdb_httplib::Server &server;
			std::thread &listener;
			~ListenerGuard() {
				server.stop();
				if (listener.joinable()) {
					listener.join();
				}
			}
		} guard {server, listener};
		server.wait_until_ready();

		DBConfig config;
		config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
		DuckDB database(nullptr, &config);
		Connection connection(database);
		auto version = Run(connection, "PRAGMA version");
		const auto actual_library_version = version->GetValue(0, 0).ToString();
		const auto actual_source_id = version->GetValue(1, 0).ToString();
		Check(actual_library_version == "v1.5.6" && actual_source_id == "069cc9f9",
		      "provider replacement core mismatch: actual library_version='" + actual_library_version +
		          "', source_id='" + actual_source_id + "'; expected v1.5.6 / 069cc9f9");
		Run(connection, "LOAD " + Quote(argv[5]));
		Run(connection, "SET httpfs_client_implementation = 'curl'");
		Run(connection, "LOAD " + Quote(argv[1]));
		auto httpfs_version = Run(connection, "SELECT extension_version FROM duckdb_extensions() "
		                                      "WHERE extension_name = 'httpfs' AND loaded");
		Check(httpfs_version->RowCount() == 1 && httpfs_version->GetValue(0, 0).ToString() == argv[6],
		      "wrong HTTPFS-Curl artifact");
		auto quackapi_version = Run(connection, "SELECT extension_version FROM duckdb_extensions() "
		                                        "WHERE extension_name = 'quackapi' AND loaded");
		Check(quackapi_version->RowCount() == 1 && quackapi_version->GetValue(0, 0).ToString() == argv[2],
		      "wrong QuackAPI artifact");
		Check(HTTPUtil::Get(*connection.context->db).GetName() == "HTTPFS-Curl", "HTTPFS-Curl did not become active");
		Run(connection, "SET quackapi_post_concurrency = 1");
		const std::string root = "http://127.0.0.1:" + std::to_string(port);
		auto first = Run(connection, "SELECT receipt.status, receipt.reused_connection FROM "
		                             "(SELECT quackapi_post(" +
		                                 Quote(root + "/echo") + ", 'first') AS receipt)");
		Check(first->GetValue(0, 0).GetValue<int>() == 200 && !first->GetValue(1, 0).GetValue<bool>(),
		      "initial HTTPFS-Curl POST did not create its own client");
		auto reused = Run(connection, "SELECT receipt.status, receipt.reused_connection FROM "
		                              "(SELECT quackapi_post(" +
		                                  Quote(root + "/echo") + ", 'second') AS receipt)");
		Check(reused->GetValue(0, 0).GetValue<int>() == 200 && reused->GetValue(1, 0).GetValue<bool>(),
		      "HTTPFS-Curl worker did not retain its client");

		Connection in_flight(database);
		std::atomic<bool> slow_ok {false};
		std::thread slow_call([&]() {
			auto result = in_flight.Query("SELECT (quackapi_post(" + Quote(root + "/slow") + ", 'slow')).status");
			slow_ok.store(!result->HasError() && result->GetValue(0, 0).GetValue<int>() == 200);
		});
		struct ThreadJoin {
			std::thread &thread;
			~ThreadJoin() {
				if (thread.joinable()) {
					thread.join();
				}
			}
		} slow_join {slow_call};
		while (!slow_active.load()) {
			std::this_thread::yield();
		}
		Connection loader(database);
		Run(loader, "LOAD " + Quote(argv[3]));
		auto curl_version = Run(loader, "SELECT extension_version FROM duckdb_extensions() "
		                                "WHERE extension_name = 'curl_httpfs' AND loaded");
		Check(curl_version->RowCount() == 1 && curl_version->GetValue(0, 0).ToString() == argv[4],
		      "wrong curl_httpfs artifact");
		Check(HTTPUtil::Get(*connection.context->db).GetName() == "MultiCurl",
		      "loading MultiCurl did not replace HTTPFS-Curl");
		slow_call.join();
		Check(slow_ok.load(), "active HTTPFS-Curl POST failed during provider replacement");
		auto after = Run(connection, "SELECT receipt.status, receipt.reused_connection FROM "
		                             "(SELECT quackapi_post(" +
		                                 Quote(root + "/echo") + ", 'after') AS receipt)");
		Check(after->GetValue(0, 0).GetValue<int>() == 200 && !after->GetValue(1, 0).GetValue<bool>(),
		      "MultiCurl reused a HTTPFS-Curl client after provider replacement");
		auto after_reuse = Run(connection, "SELECT receipt.status, receipt.reused_connection FROM "
		                                   "(SELECT quackapi_post(" +
		                                       Quote(root + "/echo") + ", 'after-reuse') AS receipt)");
		Check(after_reuse->GetValue(0, 0).GetValue<int>() == 200 && after_reuse->GetValue(1, 0).GetValue<bool>(),
		      "MultiCurl did not retain its replacement client");
		Check(received.load() == 5, "provider replacement lost or retried a POST");
		std::cout << "provider replacement passed: 5 requests\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
