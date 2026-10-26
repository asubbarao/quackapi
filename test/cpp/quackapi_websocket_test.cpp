// Integration test: CREATE STREAM … WS over a real quackapi_serve, driven by the
// WebSocket client in the same vendored httplib.
//
// 1. Push mode: each row arrives as one JSON text frame, then the server closes.
// 2. Push mode with an interval: the SELECT re-runs until the client leaves.
// 3. Reply mode: each client message runs the SELECT with $message bound;
//    path and query parameters bind as they do for routes.
// 4. Upgrades that cannot proceed are answered over HTTP: 404 (no WS stream),
//    403 (foreign Origin), 422 (missing parameter).
// 5. quackapi_stop ends a session that is waiting on its client.
#include "duckdb.hpp"
#include "httplib.hpp"

#include <chrono>
#include <cstdio>
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
			client.close();
		}

		Expect(UpgradeStatus(BASE + "/nowhere") == 404, "no WS stream at the path: 404 before the upgrade");
		Expect(UpgradeStatus(BASE + "/ticks", {{"Origin", "http://elsewhere.test"}}) == 403,
		       "foreign Origin: 403 before the upgrade");
		Expect(UpgradeStatus(BASE + "/ticks", {{"Origin", "http://127.0.0.1:29331"}}) == 101,
		       "same Origin is accepted");
		Expect(UpgradeStatus(BASE + "/rooms/lobby") == 422, "missing $nick: 422 before the upgrade");

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
