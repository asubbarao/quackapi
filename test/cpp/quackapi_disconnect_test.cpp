// Real TCP disconnect interrupts an ordinary route and leaves the server usable.
#include "duckdb.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

static std::string Query(duckdb::Connection &con, const std::string &sql) {
	auto result = con.Query(sql);
	if (result->HasError())
		throw std::runtime_error(result->GetError());
	auto chunk = result->Fetch();
	return chunk && chunk->size() ? chunk->GetValue(0, 0).ToString() : std::string();
}

int main() {
	duckdb::DuckDB db(nullptr);
	duckdb::Connection con(db);
	Query(con, "LOAD quackapi");
	Query(con, "CREATE ROUTE disconnected GET '/disconnected' AS SELECT sum(range) FROM range(100000000000)");
	Query(con, "CREATE ROUTE alive POST '/alive' AS SELECT 1 AS n");
	Query(con, "SELECT * FROM quackapi_serve(29348, host := '127.0.0.1', access_log := false, http_client := "
	           "'httplib', query_timeout_ms := 20000, cancel_on_disconnect := true)");
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		throw std::runtime_error("socket failed");
	sockaddr_in addr {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(29348);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
		throw std::runtime_error("connect failed");
	std::string request = "GET /disconnected HTTP/1.1\r\nHost: localhost\r\n\r\n";
	if (send(fd, request.data(), request.size(), 0) != static_cast<ssize_t>(request.size()))
		throw std::runtime_error("send failed");
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	close(fd);
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (Query(con, "SELECT count(*) FROM quackapi_requests(29348) WHERE path = '/disconnected'") == "0") {
		if (std::chrono::steady_clock::now() > until)
			throw std::runtime_error("disconnected request did not end promptly");
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
	if (Query(con, "SELECT (quackapi_post('http://127.0.0.1:29348/alive', '')).status") != "200")
		throw std::runtime_error("server failed follow-up request");

	// A delayed upstream observes that quackapi closed its outbound socket after
	// the downstream client disconnected. A remote server may nevertheless keep
	// computing after losing its peer, so this checks transport cancellation only.
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	int reuse = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	addr.sin_port = htons(29349);
	if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) || listen(listener, 1)) {
		throw std::runtime_error("upstream listen failed");
	}
	Query(con,
	      "CREATE ROUTE proxy POST '/proxy' AS SELECT quackapi_post('http://127.0.0.1:29349/slow', '') AS response");
	std::atomic<bool> accepted {false};
	std::atomic<bool> upstream_closed {false};
	std::thread upstream([&]() {
		int peer = accept(listener, nullptr, nullptr);
		if (peer < 0)
			return;
		timeval timeout {4, 0};
		setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		char bytes[4096];
		if (recv(peer, bytes, sizeof(bytes), 0) > 0) {
			accepted = true;
			for (;;) {
				const auto count = recv(peer, bytes, sizeof(bytes), 0);
				if (count == 0)
					upstream_closed = true;
				if (count <= 0)
					break;
			}
		}
		close(peer);
	});
	fd = socket(AF_INET, SOCK_STREAM, 0);
	addr.sin_port = htons(29348);
	if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr))) {
		close(listener);
		upstream.join();
		throw std::runtime_error("proxy connect failed");
	}
	request = "POST /proxy HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
	send(fd, request.data(), request.size(), 0);
	const auto accept_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!accepted && std::chrono::steady_clock::now() < accept_until) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	close(fd);
	if (!accepted)
		shutdown(listener, SHUT_RDWR);
	upstream.join();
	close(listener);
	if (!accepted || !upstream_closed)
		throw std::runtime_error("upstream POST socket was not cancelled");
	if (Query(con, "SELECT (quackapi_post('http://127.0.0.1:29348/alive', '')).status") != "200")
		throw std::runtime_error("server failed after cancelled upstream");
	Query(con, "SELECT * FROM quackapi_stop(29348)");
	// Default mode preserves a client that half-closes writes, then reads the response.
	Query(con, "CREATE ROUTE half_closed POST '/half-close' AS SELECT sleep_ms(100) AS waited");
	Query(con,
	      "SELECT * FROM quackapi_serve(29348, host := '127.0.0.1', access_log := false, http_client := 'httplib')");
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
		throw std::runtime_error("half-close connect failed");
	timeval receive_timeout {3, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
	request = "POST /half-close HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
	send(fd, request.data(), request.size(), 0);
	shutdown(fd, SHUT_WR);
	char response[4096];
	const auto received = recv(fd, response, sizeof(response), 0);
	close(fd);
	if (received <= 0 || std::string(response, static_cast<size_t>(received)).find("200") == std::string::npos) {
		throw std::runtime_error("default mode rejected a legal write-half-close");
	}
	Query(con, "SELECT * FROM quackapi_stop(29348)");
}
