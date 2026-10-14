// HTTPS counterpart to the plain-HTTP provider contract. The checked-in key
// belongs only to this disposable loopback fixture.
#include "duckdb.hpp"
#include "httplib.hpp"
#include "quackapi_core_identity.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <iterator>
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

static void CheckExtensionsInitiallyUnloaded(Connection &connection) {
	auto loaded = Run(connection, "SELECT count(extension_name) FROM duckdb_extensions() "
	                              "WHERE loaded AND extension_name IN ('quackapi', 'curl_httpfs', 'httpfs')");
	Check(loaded->GetValue(0, 0).GetValue<int64_t>() == 0,
	      "test core unexpectedly preloaded QuackAPI or an HTTP provider");
}

int main(int argc, char **argv) {
	try {
		Check(argc == 8, "usage: post_tls_test QUACKAPI_PATH QUACKAPI_VERSION PROVIDER_PATH PROVIDER_NAME "
		                 "PROVIDER_VERSION TEST_CERT TEST_KEY");
		std::atomic<int> received {0};
		std::atomic<int> redirects {0};
		duckdb_httplib_openssl::SSLServer server(argv[6], argv[7]);
		Check(server.is_valid(), "test TLS certificate or key is invalid");
		server.Post("/headers", [&](const duckdb_httplib_openssl::Request &request,
		                            duckdb_httplib_openssl::Response &response) {
			received.fetch_add(1);
			const auto content_types = request.headers.equal_range("Content-Type");
			response.set_content(request.get_header_value("Content-Type") + "|" + request.get_header_value("X-Trace") +
			                         "|" + std::to_string(std::distance(content_types.first, content_types.second)),
			                     "text/plain");
		});
		server.Post("/slow",
		            [&](const duckdb_httplib_openssl::Request &request, duckdb_httplib_openssl::Response &response) {
			            received.fetch_add(1);
			            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
			            response.set_content(request.body, "text/plain");
		            });
		server.Post("/redirect",
		            [&](const duckdb_httplib_openssl::Request &, duckdb_httplib_openssl::Response &response) {
			            redirects.fetch_add(1);
			            response.set_redirect("/headers", 307);
		            });
		const int port = server.bind_to_any_port("127.0.0.1");
		Check(port > 0, "could not bind loopback TLS fixture");
		std::thread listener([&]() { server.listen_after_bind(); });
		struct ListenerGuard {
			duckdb_httplib_openssl::SSLServer &server;
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
		Check(MatchesPinnedDuckDBIdentity(actual_library_version, actual_source_id),
		      "test core mismatch: actual library_version='" + actual_library_version + "', source_id='" +
		          actual_source_id +
		          "'; expected v1.5.6 and an 8- or 10-character prefix of pinned core "
		          "069cc9f9b5be802405797faecc284961b07c70ef");
		CheckExtensionsInitiallyUnloaded(connection);
		Run(connection, "LOAD " + Quote(argv[3]));
		Run(connection, "LOAD " + Quote(argv[1]));
		auto loaded = Run(connection, "SELECT extension_name, extension_version FROM duckdb_extensions() "
		                              "WHERE extension_name IN ('quackapi', " +
		                                  Quote(argv[4]) + ") AND loaded ORDER BY extension_name");
		Check(loaded->RowCount() == 2, "expected provider and QuackAPI artifacts were not both loaded");
		for (idx_t row = 0; row < 2; row++) {
			const auto name = loaded->GetValue(0, row).ToString();
			const auto expected = name == "quackapi" ? argv[2] : argv[5];
			Check(loaded->GetValue(1, row).ToString() == expected, name + " artifact version mismatch");
		}
		if (std::string(argv[4]) == "httpfs") {
			Run(connection, "SET httpfs_client_implementation = 'curl'");
		}
		Run(connection, "SET GLOBAL ca_cert_file = " + Quote(argv[6]));
		const std::string url = "https://127.0.0.1:" + std::to_string(port);
		auto custom = Run(connection, "SELECT receipt.status, receipt.body, receipt.error FROM "
		                              "(SELECT quackapi_post(" +
		                                  Quote(url + "/headers") +
		                                  ", '{}', 'application/vnd.quackapi-test+json', "
		                                  "MAP {'X-Trace': 'tls-42'}) AS receipt)");
		Check(custom->GetValue(0, 0).GetValue<int>() == 200 &&
		          custom->GetValue(1, 0).ToString() == "application/vnd.quackapi-test+json|tls-42|1",
		      "HTTPS custom Content-Type/header contract failed: " + custom->GetValue(2, 0).ToString());
		auto standard = Run(connection, "SELECT receipt.status, receipt.body FROM "
		                                "(SELECT quackapi_post(" +
		                                    Quote(url + "/headers") + ", '{}') AS receipt)");
		Check(standard->GetValue(0, 0).GetValue<int>() == 200 &&
		          standard->GetValue(1, 0).ToString() == "application/json||1",
		      "HTTPS default Content-Type contract failed");
		Check(received.load() == 2, "HTTPS POSTs were retried or lost");
		Run(connection, "CREATE ROUTE tls_deadline GET '/tls-deadline' AS SELECT "
		                "(quackapi_post(" +
		                    Quote(url + "/slow") + ", 'slow')).status AS upstream_status");
		const auto started = std::chrono::steady_clock::now();
		auto timed =
		    Run(connection, "SELECT status FROM quackapi_request('GET', '/tls-deadline', query_timeout_ms := 750)");
		const auto elapsed = std::chrono::steady_clock::now() - started;
		Check(timed->GetValue(0, 0).GetValue<int>() == 504 && elapsed < std::chrono::milliseconds(1400),
		      "HTTPS POST exceeded its 750ms route deadline");
		Check(received.load() == 3, "HTTPS timed POST was replayed");
		auto redirect = Run(connection, "SELECT receipt.status, receipt.body FROM "
		                                "(SELECT quackapi_post(" +
		                                    Quote(url + "/redirect") + ", '{}') AS receipt)");
		Check(redirect->GetValue(0, 0).GetValue<int>() == 200 &&
		          redirect->GetValue(1, 0).ToString() == "application/json||1" && redirects.load() == 1 &&
		          received.load() == 4,
		      "HTTPS 307 POST redirect did not reach its target exactly once");
		std::cout << "HTTPS provider contract passed: 4 requests\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
