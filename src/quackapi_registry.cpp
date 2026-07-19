#include "quackapi_state.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include "quackapi_server.hpp"

#include <thread>

namespace duckdb {

QuackapiState::~QuackapiState() {
	StopAllServers();
}

QuackapiState &QuackapiState::Get(DatabaseInstance &db) {
	auto state = db.GetObjectCache().GetOrCreate<QuackapiState>(CACHE_KEY);
	if (!state) {
		throw InternalException("quackapi: failed to create extension state");
	}
	return *state;
}

void QuackapiState::AddRoute(const QuackapiRoute &route, bool or_replace) {
	std::lock_guard<std::mutex> lock(routes_mutex);
	for (auto it = routes.begin(); it != routes.end(); ++it) {
		if (it->name == route.name) {
			if (!or_replace) {
				throw InvalidInputException("Route \"%s\" already exists — use CREATE OR REPLACE ROUTE", route.name);
			}
			*it = route;
			return;
		}
	}
	routes.push_back(route);
}

bool QuackapiState::DropRoute(const string &name) {
	std::lock_guard<std::mutex> lock(routes_mutex);
	for (auto it = routes.begin(); it != routes.end(); ++it) {
		if (it->name == name) {
			routes.erase(it);
			return true;
		}
	}
	return false;
}

vector<QuackapiRoute> QuackapiState::SnapshotRoutes() {
	std::lock_guard<std::mutex> lock(routes_mutex);
	return routes;
}

void QuackapiState::StartServer(DatabaseInstance &db, const string &host, int port) {
	auto key = host + ":" + std::to_string(port);
	std::lock_guard<std::mutex> lock(servers_mutex);
	if (servers.find(key) != servers.end()) {
		throw InvalidInputException("quackapi already serving on %s", key);
	}
	servers.emplace(key, make_uniq<QuackapiHttpServer>(db, host, port));
}

bool QuackapiState::StopServer(int port) {
	// Under the lock: only move the server out of the map. Never hold
	// servers_mutex across httplib teardown (Close joins the worker pool).
	unique_ptr<QuackapiHttpServer> to_destroy;
	{
		std::lock_guard<std::mutex> lock(servers_mutex);
		for (auto it = servers.begin(); it != servers.end(); ++it) {
			if (it->second->Port() == port) {
				to_destroy = std::move(it->second);
				servers.erase(it);
				break;
			}
		}
	}
	if (!to_destroy) {
		return false;
	}
	// StopAccepting is socket-close only — safe from a request-handler worker.
	to_destroy->StopAccepting();
	// Full destruction (listener + worker-pool join) must run off any httplib
	// worker thread, otherwise quackapi_stop() from inside a route self-joins.
	std::thread([srv = std::move(to_destroy)]() mutable { srv.reset(); }).detach();
	return true;
}

void QuackapiState::StopAllServers() {
	vector<unique_ptr<QuackapiHttpServer>> to_destroy;
	{
		std::lock_guard<std::mutex> lock(servers_mutex);
		for (auto &kv : servers) {
			to_destroy.push_back(std::move(kv.second));
		}
		servers.clear();
	}
	for (auto &srv : to_destroy) {
		if (srv) {
			srv->StopAccepting();
		}
	}
	for (auto &srv : to_destroy) {
		std::thread([s = std::move(srv)]() mutable { s.reset(); }).detach();
	}
}

vector<std::pair<string, int>> QuackapiState::ListServers() {
	std::lock_guard<std::mutex> lock(servers_mutex);
	vector<std::pair<string, int>> result;
	result.reserve(servers.size());
	for (auto &kv : servers) {
		result.emplace_back(kv.second->Host(), kv.second->Port());
	}
	return result;
}

} // namespace duckdb
