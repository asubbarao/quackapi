// Real HTTP regression checks for row-local POST, side effects, and the
// database-wide concurrency limit. Arguments are explicit artifact paths and
// their expected extension versions; this must never load an installed copy.
#include "duckdb.hpp"
#include "duckdb/common/http_util.hpp"
#include "httplib.hpp"
#include "quackapi_core_identity.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
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

static void VerifyCoreIdentityGuard() {
	Check(MatchesPinnedDuckDBIdentity("v1.5.6", "069cc9f9"), "core identity guard rejected the 8-character pinned ID");
	Check(MatchesPinnedDuckDBIdentity("v1.5.6", "069cc9f9b5"),
	      "core identity guard rejected the 10-character pinned ID");
	Check(!MatchesPinnedDuckDBIdentity("v1.5.5", "069cc9f9"), "core identity guard accepted the wrong version");
	Check(!MatchesPinnedDuckDBIdentity("v1.5.6", "069cc9f8"), "core identity guard accepted the wrong source ID");
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
		Check(argc == 6, "usage: quackapi_post_http_test QUACKAPI_PATH QUACKAPI_VERSION PROVIDER_PATH PROVIDER_NAME "
		                 "PROVIDER_VERSION");
		VerifyCoreIdentityGuard();
		Check(std::string(argv[4]) == "curl_httpfs" || std::string(argv[4]) == "httpfs",
		      "expected curl_httpfs or httpfs provider");
		std::atomic<int> received {0};
		std::atomic<int> ambiguous_received {0};
		std::atomic<int> redirect_received {0};
		std::atomic<int> large_received {0};
		std::atomic<int> active {0};
		std::atomic<int> peak {0};
		std::mutex peer_mutex;
		std::set<int> reuse_peer_ports;
		std::string large_json_body = "[{\"payload\":[";
		for (int i = 0; i < 10000; i++) {
			if (i > 0) {
				large_json_body += ',';
			}
			large_json_body += '0';
		}
		large_json_body += "]}]";
		duckdb_httplib::Server server;
		server.set_keep_alive_max_count(20);
		server.new_task_queue = []() {
			return new duckdb_httplib::ThreadPool(48);
		};
		server.Post("/echo", [&](const duckdb_httplib::Request &request, duckdb_httplib::Response &response) {
			received.fetch_add(1);
			if (request.body == "reuse") {
				std::lock_guard<std::mutex> guard(peer_mutex);
				reuse_peer_ports.insert(request.remote_port);
			}
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
		server.Post("/drop", [&](const duckdb_httplib::Request &, duckdb_httplib::Response &response) {
			ambiguous_received.fetch_add(1);
			response.set_content_provider(10, "text/plain",
			                              [](size_t, size_t, duckdb_httplib::DataSink &) { return false; });
		});
		server.Post("/redirect", [&](const duckdb_httplib::Request &, duckdb_httplib::Response &response) {
			redirect_received.fetch_add(1);
			response.set_redirect("/echo", 307);
		});
		server.Post("/large-json", [&](const duckdb_httplib::Request &, duckdb_httplib::Response &response) {
			large_received.fetch_add(1);
			response.set_content(large_json_body, "application/json");
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
		const auto actual_library_version = version->GetValue(0, 0).ToString();
		const auto actual_source_id = version->GetValue(1, 0).ToString();
		Check(MatchesPinnedDuckDBIdentity(actual_library_version, actual_source_id),
		      "test core mismatch: actual library_version='" + actual_library_version + "', source_id='" +
		          actual_source_id +
		          "'; expected v1.5.6 and an 8- or 10-character prefix of pinned core "
		          "069cc9f9b5be802405797faecc284961b07c70ef");
		Run(connection, "LOAD " + SqlQuote(argv[3]));
		CheckExtension(connection, argv[4], argv[5]);
		if (std::string(argv[4]) == "httpfs") {
			Run(connection, "SET httpfs_client_implementation = 'curl'");
		}
		const auto expected_provider = std::string(argv[4]) == "httpfs" ? "HTTPFS-Curl" : "MultiCurl";
		Check(HTTPUtil::Get(*connection.context->db).GetName() == expected_provider,
		      "loaded artifact did not select the expected active HTTPUtil");
		const std::string core_settings = "SELECT current_setting('threads')::VARCHAR || '|' || "
		                                  "current_setting('memory_limit')::VARCHAR || '|' || "
		                                  "current_setting('preserve_insertion_order')::VARCHAR || '|' || "
		                                  "current_setting('autoinstall_known_extensions')::VARCHAR || '|' || "
		                                  "current_setting('autoload_known_extensions')::VARCHAR";
		const std::string installed_extensions =
		    "SELECT string_agg(extension_name, ',' ORDER BY extension_name) "
		    "FROM duckdb_extensions() WHERE installed AND extension_name != 'quackapi'";
		const auto settings_before = Run(connection, core_settings)->GetValue(0, 0).ToString();
		const auto extensions_before = Run(connection, installed_extensions)->GetValue(0, 0).ToString();
		Run(connection, "LOAD " + SqlQuote(argv[1]));
		CheckExtension(connection, "quackapi", argv[2]);
		Check(Run(connection, core_settings)->GetValue(0, 0).ToString() == settings_before,
		      "loading QuackAPI changed unrelated DuckDB settings");
		Check(Run(connection, installed_extensions)->GetValue(0, 0).ToString() == extensions_before,
		      "loading QuackAPI installed another extension");
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
		CheckExtension(other_setup, argv[4], argv[5]);
		CheckExtension(other_setup, "quackapi", argv[2]);
		if (std::string(argv[4]) == "httpfs") {
			Run(other_setup, "SET httpfs_client_implementation = 'curl'");
		}
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
		Run(connection, "CREATE ROUTE post_wait GET '/post-wait' AS SELECT "
		                "(quackapi_post(" +
		                    SqlQuote(slow_url) + ", 'queued')).status AS upstream_status");
		Connection occupied(database);
		std::thread occupied_call([&]() {
			auto result = occupied.Query("SELECT (quackapi_post(" + SqlQuote(slow_url) + ", 'wide')).status");
			first_ok.store(!result->HasError() && result->GetValue(0, 0).GetValue<int>() == 200);
		});
		while (active.load() == 0) {
			std::this_thread::yield();
		}
		const auto waiting_at = std::chrono::steady_clock::now();
		auto waiting = Run(connection, "SELECT status FROM quackapi_request('GET', '/post-wait', "
		                               "query_timeout_ms := 100)");
		const auto waiting_elapsed = std::chrono::steady_clock::now() - waiting_at;
		occupied_call.join();
		Check(first_ok.load() && waiting->GetValue(0, 0).GetValue<int>() == 504 &&
		          waiting_elapsed < std::chrono::milliseconds(600),
		      "queued POST ignored its route deadline");
		Check(received.load() == 110, "expired queued POST reached the server");

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

		// Real HTTP access logs must use DuckDB's native logger and redact
		// explicitly allowed credential headers and sensitive query values.
		Run(connection, "SET enable_logging = true");
		Run(connection, "SET logging_storage = 'memory'");
		Run(connection, "SET logging_level = 'debug'");
		duckdb_httplib::Server reserve;
		const int log_port = reserve.bind_to_any_port("127.0.0.1");
		Check(log_port > 0, "could not reserve access-log fixture port");
		reserve.stop();
		Run(connection, "CREATE ROUTE log_secret GET '/log-secret' AS SELECT 1 AS ok");
		Run(connection, "SELECT * FROM quackapi_serve(" + std::to_string(log_port) +
		                    ", host := '127.0.0.1', access_log := true, log_level := 'debug', "
		                    "log_headers := 'Authorization,X-Trace', log_query := true)");
		duckdb_httplib::Client inbound("127.0.0.1", log_port);
		auto logged_response = inbound.Get("/log-secret?api_key=secret42&normal=yes",
		                                   {{"Authorization", "Bearer secret42"}, {"X-Trace", "trace-42"}});
		Check(logged_response && logged_response->status == 200, "real HTTP access-log request failed");
		auto logs = Run(connection, "SELECT message FROM duckdb_logs() WHERE contains(message, '\"type\":\"access\"') "
		                            "ORDER BY timestamp DESC");
		Check(logs->RowCount() > 0, "real HTTP request did not write a native DuckDB access log");
		const auto access_log = logs->GetValue(0, 0).ToString();
		Check(access_log.find("secret42") == std::string::npos && access_log.find("Bearer") == std::string::npos &&
		          access_log.find("[REDACTED]") != std::string::npos &&
		          access_log.find("trace-42") != std::string::npos &&
		          access_log.find("normal=yes") != std::string::npos,
		      "native access log did not redact credentials while preserving allowed metadata");
		Run(connection, "CREATE ROUTE loopback_inner POST '/loopback-inner' AS SELECT 9 AS value");
		Run(connection, "CREATE ROUTE loopback_outer GET '/loopback-outer' AS SELECT "
		                "(quackapi_post('http://127.0.0.1:" +
		                    std::to_string(log_port) + "/loopback-inner', '{}')).status AS upstream_status");
		auto loopback = Run(connection, "SELECT status, decode(body) FROM quackapi_request('GET', '/loopback-outer', "
		                                "query_timeout_ms := 1000)");
		Check(loopback->GetValue(0, 0).GetValue<int>() == 200 &&
		          loopback->GetValue(1, 0).ToString() == "[{\"upstream_status\":200}]",
		      "same-database HTTP loopback stalled or returned the wrong row");
		Run(connection, "SELECT * FROM quackapi_stop(" + std::to_string(log_port) + ")");

		// The SQL caller has already been interrupted. Database shutdown must
		// finish after the bounded provider response, even though it safely
		// drains its DB-owned worker rather than detaching it.
		auto closing_database = make_uniq<DuckDB>(nullptr, &config);
		auto closing_connection = make_uniq<Connection>(*closing_database);
		Run(*closing_connection, "LOAD " + SqlQuote(argv[3]));
		Run(*closing_connection, "LOAD " + SqlQuote(argv[1]));
		if (std::string(argv[4]) == "httpfs") {
			Run(*closing_connection, "SET httpfs_client_implementation = 'curl'");
		}
		std::atomic<bool> closing_failed {false};
		std::thread closing_call([&]() {
			auto result =
			    closing_connection->Query("SELECT (quackapi_post(" + SqlQuote(slow_url) + ", 'wide')).status");
			closing_failed.store(result->HasError());
		});
		while (active.load() == 0) {
			std::this_thread::yield();
		}
		closing_connection->Interrupt();
		closing_call.join();
		Check(closing_failed.load(), "shutdown probe did not interrupt its caller");
		closing_connection.reset();
		const auto closing_at = std::chrono::steady_clock::now();
		closing_database.reset();
		const auto closing_elapsed = std::chrono::steady_clock::now() - closing_at;
		Check(closing_elapsed < std::chrono::milliseconds(1500),
		      "database shutdown outlived a bounded interrupted provider request: elapsed_ms=" +
		          std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(closing_elapsed).count()));
		auto ambiguous = Run(connection, "SELECT receipt.status, receipt.error FROM "
		                                 "(SELECT quackapi_post(" +
		                                     SqlQuote("http://127.0.0.1:" + std::to_string(port) + "/drop") +
		                                     ", 'unsafe') AS receipt)");
		Check(ambiguous_received.load() == 1, "ambiguous POST transport failure was automatically replayed");
		Check(!ambiguous->GetValue(1, 0).IsNull(), "ambiguous POST failure lost its row-local error");
		Run(connection, "SET threads = 1");
		Run(connection, "SET quackapi_post_concurrency = 0");
		peak.store(0);
		Check(concurrent(3, "x") == 3 && peak.load() == 1,
		      "automatic POST concurrency did not follow the database's one thread");
		DBConfig reuse_config;
		reuse_config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
		DuckDB reuse_database(nullptr, &reuse_config);
		Connection reuse_connection(reuse_database);
		Run(reuse_connection, "LOAD " + SqlQuote(argv[3]));
		Run(reuse_connection, "LOAD " + SqlQuote(argv[1]));
		CheckExtension(reuse_connection, argv[4], argv[5]);
		CheckExtension(reuse_connection, "quackapi", argv[2]);
		if (std::string(argv[4]) == "httpfs") {
			Run(reuse_connection, "SET httpfs_client_implementation = 'curl'");
		}
		Check(HTTPUtil::Get(*reuse_connection.context->db).GetName() == expected_provider,
		      "reuse probe selected the wrong active HTTPUtil");
		Run(reuse_connection, "SET threads = 1");
		Run(reuse_connection, "SET quackapi_post_concurrency = 0");
		for (int i = 0; i < 10; i++) {
			auto reused = Run(reuse_connection, "SELECT (quackapi_post(" + SqlQuote(echo_url) + ", 'reuse')).status");
			Check(reused->GetValue(0, 0).GetValue<int>() == 200, "sequential keep-alive POST failed");
		}
		{
			std::lock_guard<std::mutex> guard(peer_mutex);
			Check(reuse_peer_ports.size() == 1,
			      "10 sequential POSTs used " + std::to_string(reuse_peer_ports.size()) + " peer ports");
		}

		Check(received.load() == 126, "bounded HTTP fixture lost or retried a request");
		auto redirect = Run(connection, "SELECT receipt.status, receipt.body FROM "
		                                "(SELECT quackapi_post(" +
		                                    SqlQuote("http://127.0.0.1:" + std::to_string(port) + "/redirect") +
		                                    ", 'redirect') AS receipt)");
		Check(redirect->GetValue(0, 0).GetValue<int>() == 200 && redirect->GetValue(1, 0).ToString() == "redirect" &&
		          redirect_received.load() == 1 && received.load() == 127,
		      "HTTP 307 POST redirect did not reach its target exactly once");

		// The table overload must not reintroduce the old 32-request cap.
		Run(connection, "SET threads = 4");
		Run(connection, "SET quackapi_post_concurrency = 40");
		peak.store(0);
		auto table_wide = Run(connection, "SELECT i, response.status, response.body FROM quackapi_post((SELECT i, " +
		                                      SqlQuote(slow_url) +
		                                      " AS url, 'wide' AS body FROM range(40) t(i))) ORDER BY i");
		Check(table_wide->RowCount() == 40, "table POST dropped an input row");
		for (idx_t i = 0; i < 40; i++) {
			Check(table_wide->GetValue(0, i).GetValue<int64_t>() == static_cast<int64_t>(i) &&
			          table_wide->GetValue(1, i).GetValue<int>() == 200 &&
			          table_wide->GetValue(2, i).ToString() == "wide",
			      "table POST crossed or lost rows above concurrency 32");
		}
		Check(peak.load() > 32 && peak.load() <= 40 && received.load() == 167,
		      "table POST ignored its configured concurrency above 32");

		// Separate table callers share the database's limit, while a second
		// DuckDB instance has its own limit even with the same HTTP provider.
		Run(connection, "SET quackapi_post_concurrency = 2");
		peak.store(0);
		std::atomic<int> table_ready {0};
		std::atomic<bool> table_go {false};
		std::atomic<int> table_passed {0};
		auto table_caller = [&]() {
			Connection caller(database);
			table_ready.fetch_add(1);
			while (!table_go.load()) {
				std::this_thread::yield();
			}
			auto result = caller.Query("SELECT count(i) FROM quackapi_post((SELECT i, " + SqlQuote(slow_url) +
			                           " AS url, 'wide' AS body FROM range(3) t(i))) WHERE response.status = 200");
			if (!result->HasError() && result->GetValue(0, 0).GetValue<int64_t>() == 3) {
				table_passed.fetch_add(1);
			}
		};
		std::thread table_first(table_caller);
		std::thread table_second(table_caller);
		while (table_ready.load() != 2) {
			std::this_thread::yield();
		}
		table_go.store(true);
		table_first.join();
		table_second.join();
		Check(table_passed.load() == 2 && peak.load() > 1 && peak.load() <= 2 && received.load() == 173,
		      "table POST callers did not share the database-wide limit");

		Run(connection, "SET quackapi_post_concurrency = 1");
		Run(other_setup, "SET quackapi_post_concurrency = 1");
		peak.store(0);
		std::atomic<int> table_independent_ready {0};
		std::atomic<bool> table_independent_go {false};
		std::atomic<int> table_independent_passed {0};
		auto table_database_call = [&](DuckDB &target) {
			Connection caller(target);
			table_independent_ready.fetch_add(1);
			while (!table_independent_go.load()) {
				std::this_thread::yield();
			}
			auto result = caller.Query("SELECT response.status FROM quackapi_post((SELECT " + SqlQuote(slow_url) +
			                           " AS url, 'wide' AS body))");
			if (!result->HasError() && result->GetValue(0, 0).GetValue<int>() == 200) {
				table_independent_passed.fetch_add(1);
			}
		};
		std::thread table_db_first([&]() { table_database_call(database); });
		std::thread table_db_second([&]() { table_database_call(other_database); });
		while (table_independent_ready.load() != 2) {
			std::this_thread::yield();
		}
		table_independent_go.store(true);
		table_db_first.join();
		table_db_second.join();
		Check(table_independent_passed.load() == 2 && peak.load() == 2 && received.load() == 175,
		      "independent databases shared the table POST concurrency limit");

		// Interrupt an in-flight table batch before it can admit the next row.
		Connection table_interrupted(database);
		std::atomic<bool> table_cancelled {false};
		std::thread table_call([&]() {
			auto result = table_interrupted.Query("SELECT count(i) FROM quackapi_post((SELECT i, " +
			                                      SqlQuote(slow_url) +
			                                      " AS url, 'wide' AS body FROM range(33) t(i)))");
			table_cancelled.store(result->HasError());
		});
		const auto table_started_by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (active.load() == 0 && std::chrono::steady_clock::now() < table_started_by) {
			std::this_thread::yield();
		}
		if (active.load() == 0) {
			table_interrupted.Interrupt();
			table_call.join();
			Check(false, "table POST never reached the slow HTTP fixture");
		}
		const auto table_cancel_at = std::chrono::steady_clock::now();
		table_interrupted.Interrupt();
		table_call.join();
		const auto table_cancel_elapsed = std::chrono::steady_clock::now() - table_cancel_at;
		const auto table_drained_by = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (active.load() != 0 && std::chrono::steady_clock::now() < table_drained_by) {
			std::this_thread::yield();
		}
		Check(active.load() == 0 && table_cancelled.load() &&
		          table_cancel_elapsed < std::chrono::milliseconds(600) &&
		          received.load() == 176,
		      "interrupted table POST did not cancel promptly or admitted another row");
		auto table_headers = Run(connection, "SELECT response.status, response.body FROM quackapi_post((SELECT " +
		                                         SqlQuote(headers_url) +
		                                         " AS url, '{}'::JSON AS body, "
		                                         "'application/vnd.quackapi-test+json' AS content_type, "
		                                         "MAP {'X-Trace': 'table-42'} AS headers))");
		Check(table_headers->GetValue(0, 0).GetValue<int>() == 200 &&
		          table_headers->GetValue(1, 0).ToString() == "application/vnd.quackapi-test+json|table-42|1",
		      "table POST changed typed body, Content-Type or custom headers");
		auto table_default_headers = Run(connection, "SELECT response.status, response.body FROM "
		                                                 "quackapi_post((SELECT " +
		                                                     SqlQuote(headers_url) + " AS url, '{}' AS body))");
		Check(table_default_headers->GetValue(0, 0).GetValue<int>() == 200 &&
		          table_default_headers->GetValue(1, 0).ToString() == "application/json||1" &&
		          received.load() == 178,
		      "table POST changed default Content-Type or request count");

		// A full scalar response vector exhausts this limit. The table operator
		// must let downstream JSON parsing consume a bounded batch at a time.
		DBConfig memory_config;
		memory_config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
		DuckDB memory_database(nullptr, &memory_config);
		Connection memory_connection(memory_database);
		Run(memory_connection, "LOAD " + SqlQuote(argv[3]));
		Run(memory_connection, "LOAD " + SqlQuote(argv[1]));
		CheckExtension(memory_connection, argv[4], argv[5]);
		CheckExtension(memory_connection, "quackapi", argv[2]);
		if (std::string(argv[4]) == "httpfs") {
			Run(memory_connection, "SET httpfs_client_implementation = 'curl'");
		}
		Run(memory_connection, "LOAD json");
		Run(memory_connection, "SET threads = 4");
		Run(memory_connection, "SET memory_limit = '128MB'");
		Run(memory_connection, "SET quackapi_post_concurrency = 4");
		auto large_rows = Run(memory_connection,
		                      "SELECT count(i), sum(json_array_length(response.body::JSON, '$[0].payload')) "
		                      "FROM quackapi_post((SELECT i, " +
		                          SqlQuote("http://127.0.0.1:" + std::to_string(port) + "/large-json") +
		                          " AS url, '{}' AS body FROM range(256) t(i)))");
		Check(large_rows->GetValue(0, 0).GetValue<int64_t>() == 256 &&
		          large_rows->GetValue(1, 0).GetValue<int64_t>() == 2560000 && large_received.load() == 256,
		      "table POST did not stream 256 large JSON bodies through downstream parsing under 128 MB");
		std::cout << "POST HTTP integration passed: 178 requests including table batches\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
