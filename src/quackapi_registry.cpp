#include "quackapi_state.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include "quackapi_server.hpp"

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
	std::lock_guard<std::mutex> lock(servers_mutex);
	for (auto it = servers.begin(); it != servers.end(); ++it) {
		if (it->second->Port() == port) {
			it->second->Close();
			servers.erase(it);
			return true;
		}
	}
	return false;
}

void QuackapiState::StopAllServers() {
	std::lock_guard<std::mutex> lock(servers_mutex);
	for (auto &kv : servers) {
		kv.second->Close();
	}
	servers.clear();
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
