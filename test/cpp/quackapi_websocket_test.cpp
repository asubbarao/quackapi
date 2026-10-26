// Integration test: CREATE STREAM … WS over a real quackapi_serve, driven by the
// WebSocket client in the same vendored httplib.
//
// 1. Push mode: each row arrives as one JSON text frame, then the server closes.
// 2. Push mode with an interval: the SELECT re-runs until the client leaves.
// 3. Reply mode: each client message runs the SELECT with $message bound;
//    path and query parameters bind as they do for routes.
//    A message that cannot bind (a binary frame that is not UTF-8) gets an
//    error reply and the session continues.
// 4. Upgrades that cannot proceed are answered over HTTP: 404 (no WS stream),
//    403 (foreign Origin), 422 (missing parameter), 503 (sessions past half
//    the workers), while HTTP keeps answering.
// 5. A static 304 still runs quackapi's file handler (re-vendored httplib).
// 6. quackapi_stop ends a session that is waiting on its client.
#include "duckdb.hpp"
#include "httplib.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using clock_type = std::chrono::steady_clock;
namespace ws = quackapi_httplib::ws;

const std::string BASE = "ws://127.0.0.1:29331";

void Run(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	if (result->HasError()) {
		throw std::runtime_error(sql + ": " + result->GetError());
	}
}

void Expect(bool ok, const std::string &what) {
	if (!ok) {
		throw std::runtime_error("FAILED: " + what);
	}
	printf("ok: %s\n", what.c_str());
}

std::string Read(ws::WebSocketClient &client) {
	std::string message;
	if (client.read(message) == ws::Fail) {
		return "<closed>";
	}
	return message;
}

int UpgradeStatus(const std::string &url, const quackapi_httplib::Headers &headers = {}) {
	ws::WebSocketClient client(url, headers);
	auto result = client.connect();
	return result ? 101 : result.status();
}
} // namespace

int main() {
	setvbuf(stdout, nullptr, _IONBF, 0);
	try {
		duckdb::DuckDB database(nullptr);
		duckdb::Connection connection(database);
		Run(connection, "LOAD quackapi");
		Run(connection, "CREATE STREAM ticks WS '/ticks' AS SELECT i AS id, 'tick' AS msg FROM range(3) t(i)");
		Run(connection, "CREATE STREAM pulse WS '/pulse' WITH (interval='100ms') AS SELECT 1 AS n");
		Run(connection, "CREATE STREAM chat WS '/rooms/:room' AS "
		                "SELECT $room AS room, $nick AS nick, upper($message) AS said");
		Run(connection, "SELECT * FROM quackapi_serve(29331, host := '127.0.0.1', access_log := false)");

		{
			ws::WebSocketClient client(BASE + "/ticks");
			Expect(static_cast<bool>(client.connect()), "push stream accepts the upgrade");
			Expect(Read(client) == R"({"id":0,"msg":"tick"})", "row 0 is one JSON frame");
			Expect(Read(client) == R"({"id":1,"msg":"tick"})", "row 1 is one JSON frame");
			Expect(Read(client) == R"({"id":2,"msg":"tick"})", "row 2 is one JSON frame");
			Expect(Read(client) == "<closed>", "server closes after the last row");
		}

		{
			ws::WebSocketClient client(BASE + "/pulse");
			Expect(static_cast<bool>(client.connect()), "interval stream accepts the upgrade");
			const auto start = clock_type::now();
			Expect(Read(client) == R"({"n":1})" && Read(client) == R"({"n":1})" && Read(client) == R"({"n":1})",
			       "interval stream re-runs its SELECT");
			Expect(clock_type::now() - start >= std::chrono::milliseconds(150), "re-runs wait for the interval");
			client.close();
		}

		{
			ws::WebSocketClient client(BASE + "/rooms/lobby?nick=ada");
			Expect(static_cast<bool>(client.connect()), "reply stream accepts the upgrade");
			client.send("hello");
			Expect(Read(client) == R"({"room":"lobby","nick":"ada","said":"HELLO"})",
			       "a message runs the SELECT with $message, path and query bound");
			client.send("again");
			Expect(Read(client) == R"({"room":"lobby","nick":"ada","said":"AGAIN"})", "every message gets a reply");
			// Binary frames skip httplib's UTF-8 check; binding one must not take the process down.
			const char invalid_utf8[] = {'\xff', '\xfe'};
			client.send(invalid_utf8, sizeof(invalid_utf8));
			Expect(Read(client).find("\"detail\"") != std::string::npos, "an unbindable message gets an error reply");
			client.send("still here");
			Expect(Read(client) == R"({"room":"lobby","nick":"ada","said":"STILL HERE"})",
			       "the session continues after an unbindable message");
			client.close();
		}

		Expect(UpgradeStatus(BASE + "/nowhere") == 404, "no WS stream at the path: 404 before the upgrade");
		Expect(UpgradeStatus(BASE + "/ticks", {{"Origin", "http://elsewhere.test"}}) == 403,
		       "foreign Origin: 403 before the upgrade");
		Expect(UpgradeStatus(BASE + "/ticks", {{"Origin", "http://127.0.0.1:29331"}}) == 101,
		       "same Origin is accepted");
		Expect(UpgradeStatus(BASE + "/rooms/lobby") == 422, "missing $nick: 422 before the upgrade");

		{
			// worker_threads := 2 admits one session, so HTTP keeps a worker.
			Run(connection, "SELECT * FROM quackapi_serve(29332, host := '127.0.0.1', access_log := false, "
			                "worker_threads := 2)");
			ws::WebSocketClient first("ws://127.0.0.1:29332/rooms/lobby?nick=ada");
			Expect(static_cast<bool>(first.connect()), "first session is admitted");
			Expect(UpgradeStatus("ws://127.0.0.1:29332/rooms/lobby?nick=bob") == 503,
			       "a session past half the workers gets 503");
			quackapi_httplib::Client http("127.0.0.1", 29332);
			http.set_read_timeout(3, 0);
			auto health = http.Get("/health");
			Expect(health && health->status == 200, "HTTP still answers while sessions are open");
			first.close();
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			Expect(UpgradeStatus("ws://127.0.0.1:29332/rooms/lobby?nick=bob") == 101,
			       "a closed session frees its place");
			Run(connection, "SELECT * FROM quackapi_stop(29332)");
		}

		{
			// The 0.53 304 path returned before quackapi's file handler, which stamps
			// X-Request-ID and marks the response for the access log.
			auto dir = std::filesystem::temp_directory_path() / "quackapi_websocket_test_static";
			std::filesystem::create_directories(dir);
			std::ofstream(dir / "index.txt") << "hello";
			Run(connection, "SELECT * FROM quackapi_serve(29333, host := '127.0.0.1', access_log := false, "
			                "static_dir := '" +
			                    dir.string() + "')");
			quackapi_httplib::Client http("127.0.0.1", 29333);
			auto first = http.Get("/index.txt");
			Expect(first && first->status == 200 && first->has_header("X-Request-ID"), "static 200 is stamped");
			auto again =
			    http.Get("/index.txt", quackapi_httplib::Headers {{"If-None-Match", first->get_header_value("ETag")}});
			Expect(again && again->status == 304 && again->has_header("X-Request-ID"), "static 304 is stamped too");
			Run(connection, "SELECT * FROM quackapi_stop(29333)");
			std::filesystem::remove_all(dir);
		}

		{
			ws::WebSocketClient client(BASE + "/rooms/lobby?nick=ada");
			// Without the server's close, read() fails at this timeout instead of hanging the test.
			client.set_read_timeout(3, 0);
			Expect(static_cast<bool>(client.connect()), "reply stream for the shutdown check");
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			const auto start = clock_type::now();
			Run(connection, "SELECT * FROM quackapi_stop(29331)");
			Expect(Read(client) == "<closed>", "quackapi_stop closes an idle session");
			const auto closed_ms =
			    std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - start).count();
			printf("session closed %lld ms after quackapi_stop\n", static_cast<long long>(closed_ms));
			Expect(closed_ms < 1500, "the session closes within one poll, not at a read timeout");
		}
	} catch (std::exception &ex) {
		fprintf(stderr, "%s\n", ex.what());
		return 1;
	}
	return 0;
}
