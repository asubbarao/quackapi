#pragma once

#include <thread>
#include <vector>

#include "duckdb/common/helper.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unique_ptr.hpp"

namespace duckdb_httplib {
class Server;
struct Request;
struct Response;
} // namespace duckdb_httplib

namespace duckdb {

class DatabaseInstance;

//! HTTP server that dispatches requests to routes in QuackapiState.
//! Transport is DuckDB's bundled httplib, following the same listener-thread +
//! synchronous-bind discipline as the core quack extension's RPC server.
class QuackapiHttpServer {
public:
	QuackapiHttpServer(DatabaseInstance &db, const string &host, int port);
	~QuackapiHttpServer();

	//! Close the listener socket only; safe from a request-handler thread.
	void StopAccepting();
	//! Stop accepting AND join listener threads. Must not be called from a
	//! worker thread (httplib's listen teardown joins all workers).
	void Close();

	const string &Host() const {
		return host;
	}
	int Port() const {
		return port;
	}

private:
	static void ListenThread(QuackapiHttpServer *server);
	void HandleRequest(const duckdb_httplib::Request &req, duckdb_httplib::Response &res);

	weak_ptr<DatabaseInstance> db_ptr;
	string host;
	int port;
	unique_ptr<duckdb_httplib::Server> server;
	std::vector<std::thread> listen_threads;
	bool is_running = false;
};

} // namespace duckdb
