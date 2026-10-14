#include "quackapi_state.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
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

void QuackapiState::PublishRoutes() {
	// Caller holds routes_mutex.
	routes_live = make_shared_ptr<const vector<QuackapiRoute>>(routes);
}

void QuackapiState::AddRoute(const QuackapiRoute &route, bool or_replace) {
	std::lock_guard<std::mutex> lock(routes_mutex);
	// Method+pattern must be unique across route names. Otherwise OR REPLACE
	// onto another route's path leaves duplicate handlers and OpenAPI keys.
	auto collides = [&](const QuackapiRoute &candidate, const string &ignore_name) -> const QuackapiRoute * {
		for (auto &other : routes) {
			if (other.name == ignore_name) {
				continue;
			}
			if (other.method == candidate.method && other.pattern == candidate.pattern) {
				return &other;
			}
		}
		return nullptr;
	};
	for (auto it = routes.begin(); it != routes.end(); ++it) {
		if (it->name == route.name) {
			if (!or_replace) {
				throw InvalidInputException("Route \"%s\" already exists — use CREATE OR REPLACE ROUTE", route.name);
			}
			if (auto *hit = collides(route, route.name)) {
				throw InvalidInputException("Route path \"%s %s\" already used by route \"%s\"", route.method,
				                            route.pattern, hit->name);
			}
			*it = route;
			PublishRoutes();
			// OR REPLACE is a new registration — do not inherit spent rate-limit windows.
			ClearRouteRateLimit(route.name);
			return;
		}
	}
	if (auto *hit = collides(route, string())) {
		throw InvalidInputException("Route path \"%s %s\" already used by route \"%s\"", route.method, route.pattern,
		                            hit->name);
	}
	routes.push_back(route);
	PublishRoutes();
	ClearRouteRateLimit(route.name);
}

bool QuackapiState::DropRoute(const string &name) {
	std::lock_guard<std::mutex> lock(routes_mutex);
	for (auto it = routes.begin(); it != routes.end(); ++it) {
		if (it->name == name) {
			routes.erase(it);
			PublishRoutes();
			ClearRouteRateLimit(name);
			return true;
		}
	}
	return false;
}

vector<QuackapiRoute> QuackapiState::SnapshotRoutes() {
	auto live = LiveRoutes();
	return *live;
}

shared_ptr<const vector<QuackapiRoute>> QuackapiState::LiveRoutes() {
	std::lock_guard<std::mutex> lock(routes_mutex);
	if (!routes_live) {
		PublishRoutes();
	}
	return routes_live;
}

void QuackapiState::AddGroup(const QuackapiGroup &group, bool or_replace) {
	std::lock_guard<std::mutex> lock(groups_mutex);
	for (auto it = groups.begin(); it != groups.end(); ++it) {
		if (it->name == group.name) {
			if (!or_replace) {
				throw InvalidInputException("Group \"%s\" already exists — use CREATE OR REPLACE GROUP", group.name);
			}
			*it = group;
			return;
		}
	}
	groups.push_back(group);
}

bool QuackapiState::DropGroup(const string &name) {
	std::lock_guard<std::mutex> lock(groups_mutex);
	for (auto it = groups.begin(); it != groups.end(); ++it) {
		if (it->name == name) {
			groups.erase(it);
			return true;
		}
	}
	return false;
}

bool QuackapiState::GetGroup(const string &name, QuackapiGroup &out) {
	std::lock_guard<std::mutex> lock(groups_mutex);
	for (auto &group : groups) {
		if (group.name == name) {
			out = group;
			return true;
		}
	}
	return false;
}

vector<QuackapiGroup> QuackapiState::SnapshotGroups() {
	std::lock_guard<std::mutex> lock(groups_mutex);
	return groups;
}

void QuackapiState::AddAuth(const QuackapiAuth &auth, bool or_replace) {
	std::lock_guard<std::mutex> lock(auths_mutex);
	// Match REQUIRE / GetAuth: scheme names are case-insensitive.
	for (auto it = auths.begin(); it != auths.end(); ++it) {
		if (StringUtil::CIEquals(it->name, auth.name)) {
			if (!or_replace) {
				throw InvalidInputException("Auth \"%s\" already exists — use CREATE OR REPLACE AUTH", auth.name);
			}
			const string old_name = it->name;
			const auto old_kind = it->kind;
			// Hashed API keys must not outlive an API_KEY scheme (OR REPLACE to JWT
			// used to leave keys that resurrected after OR REPLACE back to API_KEY).
			if (old_kind == QuackapiAuthKind::API_KEY && auth.kind != QuackapiAuthKind::API_KEY) {
				api_keys.erase(std::remove_if(api_keys.begin(), api_keys.end(),
				                              [&](const QuackapiApiKeyEntry &e) {
					                              return StringUtil::CIEquals(e.auth_name, old_name);
				                              }),
				               api_keys.end());
			} else if (old_name != auth.name) {
				// Preserve keys across casing-only renames of the same API_KEY scheme.
				for (auto &entry : api_keys) {
					if (StringUtil::CIEquals(entry.auth_name, old_name)) {
						entry.auth_name = auth.name;
					}
				}
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
	string dropped_name;
	for (auto it = auths.begin(); it != auths.end(); ++it) {
		if (StringUtil::CIEquals(it->name, name)) {
			dropped_name = it->name;
			auths.erase(it);
			found = true;
			break;
		}
	}
	if (found) {
		// Drop associated API keys so hashes cannot outlive their scheme.
		// Case-insensitive: DROP AUTH site must clear keys for CREATE AUTH Site.
		api_keys.erase(std::remove_if(api_keys.begin(), api_keys.end(),
		                              [&](const QuackapiApiKeyEntry &e) {
			                              return StringUtil::CIEquals(e.auth_name, dropped_name);
		                              }),
		               api_keys.end());
	}
	return found;
}

bool QuackapiState::GetAuth(const string &name, QuackapiAuth &out) {
	std::lock_guard<std::mutex> lock(auths_mutex);
	// Case-insensitive lookup: REQUIRE <scheme> must resolve regardless of the
	// case used at CREATE AUTH time or in the route DDL — a mismatch here must
	// never surface as a 500 (fuzz catalog P1 "wrong-case REQUIRE name").
	for (auto &auth : auths) {
		if (StringUtil::CIEquals(auth.name, name)) {
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
		if (StringUtil::CIEquals(auth.name, auth_name)) {
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
	// Store the canonical scheme name from the registry (not the caller's casing).
	entry.auth_name = auth_ptr->name;
	entry.key_hash = QuackapiSha256(raw_key);
	entry.subject = subject;
	// Replace existing key with the same hash (same raw key re-added).
	for (auto &existing : api_keys) {
		if (StringUtil::CIEquals(existing.auth_name, entry.auth_name) &&
		    QuackapiConstantTimeEquals(existing.key_hash, entry.key_hash)) {
			existing.subject = subject;
			existing.auth_name = entry.auth_name;
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
		if (StringUtil::CIEquals(entry.auth_name, auth_name)) {
			result.push_back(entry);
		}
	}
	return result;
}

void QuackapiState::StartServer(DatabaseInstance &db, const string &host, int port, const QuackapiServeOptions &opts) {
	// Mirrors QuackStorageExtensionInfo::CreateServer (duckdb-quack
	// src/quack_storage.cpp): lock map → reject duplicate key → construct
	// (bind happens in ctor so EADDRINUSE propagates) → emplace.
	auto key = host + ":" + std::to_string(port);
	std::lock_guard<std::mutex> lock(servers_mutex);
	if (servers.find(key) != servers.end()) {
		throw InvalidInputException("quackapi already serving on %s", key);
	}
	servers.emplace(key, make_uniq<QuackapiHttpServer>(db, host, port, opts));
}

bool QuackapiState::StopServer(int port) {
	// Mirrors QuackStorageExtensionInfo::StopServer (duckdb-quack
	// src/quack_storage.cpp): under lock move out of map; StopAccepting
	// (socket only); full destroy on a detached thread so httplib worker-pool
	// join never runs under servers_mutex or on a request-handler thread.
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
	to_destroy->StopAccepting();
	// Brief delay so a self-stop from inside a route can finish its response
	// before ~Server joins the thread pool.
	std::thread(
	    [](unique_ptr<QuackapiHttpServer> srv) {
		    std::this_thread::sleep_for(std::chrono::milliseconds(100));
		    srv.reset();
	    },
	    std::move(to_destroy))
	    .detach();
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
	// Stop accepting before joining listener and request-handler threads.
	for (auto &srv : to_destroy) {
		if (srv) {
			srv->StopAccepting();
		}
	}
	for (auto &srv : to_destroy) {
		std::thread(
		    [](unique_ptr<QuackapiHttpServer> s) {
			    std::this_thread::sleep_for(std::chrono::milliseconds(100));
			    s.reset();
		    },
		    std::move(srv))
		    .detach();
	}
}

bool QuackapiState::HasServerOnPort(int port) {
	std::lock_guard<std::mutex> lock(servers_mutex);
	for (auto &kv : servers) {
		if (kv.second->Port() == port) {
			return true;
		}
	}
	return false;
}

bool QuackapiState::GetServerHost(int port, string &host_out) {
	std::lock_guard<std::mutex> lock(servers_mutex);
	for (auto &kv : servers) {
		if (kv.second->Port() == port) {
			host_out = kv.second->Host();
			return true;
		}
	}
	return false;
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

vector<QuackapiRequestRecord> QuackapiState::SnapshotRequests(int port) {
	std::lock_guard<std::mutex> lock(servers_mutex);
	if (port == 0) {
		if (servers.empty()) {
			return {};
		}
		if (servers.size() > 1) {
			throw InvalidInputException("quackapi_requests(): port is required when multiple servers are running");
		}
		return servers.begin()->second->SnapshotRequests();
	}
	for (auto &kv : servers) {
		if (kv.second->Port() == port) {
			return kv.second->SnapshotRequests();
		}
	}
	return {};
}

vector<QuackapiTelemetryStatus> QuackapiState::SnapshotTelemetryStatus(int port) {
	std::lock_guard<std::mutex> lock(servers_mutex);
	if (port == 0) {
		if (servers.empty()) {
			return {};
		}
		if (servers.size() > 1) {
			throw InvalidInputException(
			    "quackapi_telemetry_status(): port is required when multiple servers are running");
		}
		return servers.begin()->second->SnapshotTelemetryStatus();
	}
	for (auto &kv : servers) {
		if (kv.second->Port() == port) {
			return kv.second->SnapshotTelemetryStatus();
		}
	}
	return {};
}

bool QuackapiState::FlushTelemetry(int port) {
	std::lock_guard<std::mutex> lock(servers_mutex);
	if (port == 0) {
		if (servers.empty()) {
			return false;
		}
		if (servers.size() > 1) {
			throw InvalidInputException(
			    "quackapi_telemetry_flush(): port is required when multiple servers are running");
		}
		servers.begin()->second->FlushTelemetryPending();
		return true;
	}
	for (auto &kv : servers) {
		if (kv.second->Port() == port) {
			kv.second->FlushTelemetryPending();
			return true;
		}
	}
	return false;
}

} // namespace duckdb
