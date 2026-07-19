#include "quackapi_state.hpp"

#include <algorithm>
#include <thread>

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include "quackapi_auth.hpp"
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

void QuackapiState::AddAuth(const QuackapiAuth &auth, bool or_replace) {
	std::lock_guard<std::mutex> lock(auths_mutex);
	for (auto it = auths.begin(); it != auths.end(); ++it) {
		if (it->name == auth.name) {
			if (!or_replace) {
				throw InvalidInputException("Auth \"%s\" already exists — use CREATE OR REPLACE AUTH", auth.name);
			}
			*it = auth;
			return;
		}
	}
	auths.push_back(auth);
}

bool QuackapiState::DropAuth(const string &name) {
	std::lock_guard<std::mutex> lock(auths_mutex);
	bool found = false;
	for (auto it = auths.begin(); it != auths.end(); ++it) {
		if (it->name == name) {
			auths.erase(it);
			found = true;
			break;
		}
	}
	if (found) {
		// Drop associated API keys so hashes cannot outlive their scheme.
		api_keys.erase(std::remove_if(api_keys.begin(), api_keys.end(),
		                              [&](const QuackapiApiKeyEntry &e) { return e.auth_name == name; }),
		               api_keys.end());
	}
	return found;
}

bool QuackapiState::GetAuth(const string &name, QuackapiAuth &out) {
	std::lock_guard<std::mutex> lock(auths_mutex);
	for (auto &auth : auths) {
		if (auth.name == name) {
			out = auth;
			return true;
		}
	}
	return false;
}

vector<QuackapiAuth> QuackapiState::SnapshotAuths() {
	std::lock_guard<std::mutex> lock(auths_mutex);
	return auths;
}

string QuackapiState::AddApiKey(const string &auth_name, const string &raw_key, const string &subject) {
	if (raw_key.empty()) {
		throw InvalidInputException("quackapi_add_api_key: raw_key must not be empty");
	}
	if (subject.empty()) {
		throw InvalidInputException("quackapi_add_api_key: subject must not be empty");
	}
	// Validate auth scheme under the same lock as the key insert.
	std::lock_guard<std::mutex> lock(auths_mutex);
	const QuackapiAuth *auth_ptr = nullptr;
	for (auto &auth : auths) {
		if (auth.name == auth_name) {
			auth_ptr = &auth;
			break;
		}
	}
	if (!auth_ptr) {
		throw InvalidInputException("Auth \"%s\" does not exist", auth_name);
	}
	if (auth_ptr->kind != QuackapiAuthKind::API_KEY) {
		throw InvalidInputException("Auth \"%s\" is not an API_KEY scheme", auth_name);
	}
	QuackapiApiKeyEntry entry;
	entry.auth_name = auth_name;
	entry.key_hash = QuackapiSha256(raw_key);
	entry.subject = subject;
	// Replace existing key with the same hash (same raw key re-added).
	for (auto &existing : api_keys) {
		if (existing.auth_name == auth_name && QuackapiConstantTimeEquals(existing.key_hash, entry.key_hash)) {
			existing.subject = subject;
			return subject;
		}
	}
	api_keys.push_back(std::move(entry));
	return subject;
}

vector<QuackapiApiKeyEntry> QuackapiState::SnapshotApiKeys(const string &auth_name) {
	std::lock_guard<std::mutex> lock(auths_mutex);
	vector<QuackapiApiKeyEntry> result;
	for (auto &entry : api_keys) {
		if (entry.auth_name == auth_name) {
			result.push_back(entry);
		}
	}
	return result;
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
