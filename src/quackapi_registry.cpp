#include "quackapi_state.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "duckdb/common/exception.hpp"
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

void QuackapiState::AddStream(const QuackapiStream &stream, bool or_replace) {
	std::lock_guard<std::mutex> lock(streams_mutex);
	for (auto it = streams.begin(); it != streams.end(); ++it) {
		if (it->name == stream.name) {
			if (!or_replace) {
				throw InvalidInputException("Stream \"%s\" already exists — use CREATE OR REPLACE STREAM",
				                            stream.name);
			}
			*it = stream;
			return;
		}
	}
	streams.push_back(stream);
}

bool QuackapiState::DropStream(const string &name) {
	std::lock_guard<std::mutex> lock(streams_mutex);
	for (auto it = streams.begin(); it != streams.end(); ++it) {
		if (it->name == name) {
			streams.erase(it);
			return true;
		}
	}
	return false;
}

vector<QuackapiStream> QuackapiState::SnapshotStreams() {
	std::lock_guard<std::mutex> lock(streams_mutex);
	return streams;
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

void QuackapiState::AddQueue(const QuackapiQueue &queue, bool or_replace) {
	std::lock_guard<std::mutex> lock(queues_mutex);
	for (auto it = queues.begin(); it != queues.end(); ++it) {
		if (it->name == queue.name) {
			if (!or_replace) {
				throw InvalidInputException("Queue \"%s\" already exists — use CREATE OR REPLACE QUEUE", queue.name);
			}
			*it = queue;
			return;
		}
	}
	queues.push_back(queue);
}

bool QuackapiState::DropQueue(const string &name) {
	std::lock_guard<std::mutex> lock(queues_mutex);
	for (auto it = queues.begin(); it != queues.end(); ++it) {
		if (it->name == name) {
			queues.erase(it);
			return true;
		}
	}
	return false;
}

bool QuackapiState::GetQueue(const string &name, QuackapiQueue &out) {
	std::lock_guard<std::mutex> lock(queues_mutex);
	for (auto &q : queues) {
		if (q.name == name) {
			out = q;
			return true;
		}
	}
	return false;
}

vector<QuackapiQueue> QuackapiState::SnapshotQueues() {
	std::lock_guard<std::mutex> lock(queues_mutex);
	return queues;
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
	std::thread([srv = std::move(to_destroy)]() mutable {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		srv.reset();
	}).detach();
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
		std::thread([s = std::move(srv)]() mutable {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			s.reset();
		}).detach();
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

//===--------------------------------------------------------------------===//
// Row access + masking policies
//===--------------------------------------------------------------------===//

void QuackapiState::AddRowAccessPolicy(const QuackapiRowAccessPolicy &policy, bool or_replace) {
	std::lock_guard<std::mutex> lock(policies_mutex);
	for (auto it = row_access_policies.begin(); it != row_access_policies.end(); ++it) {
		if (it->name == policy.name) {
			if (!or_replace) {
				throw InvalidInputException(
				    "Row access policy \"%s\" already exists — use CREATE OR REPLACE ROW ACCESS POLICY", policy.name);
			}
			*it = policy;
			return;
		}
	}
	// Name must not collide with a masking policy.
	for (auto &m : masking_policies) {
		if (m.name == policy.name) {
			throw InvalidInputException("Policy name \"%s\" is already used by a masking policy", policy.name);
		}
	}
	row_access_policies.push_back(policy);
}

bool QuackapiState::DropRowAccessPolicy(const string &name) {
	std::lock_guard<std::mutex> lock(policies_mutex);
	bool found = false;
	for (auto it = row_access_policies.begin(); it != row_access_policies.end(); ++it) {
		if (it->name == name) {
			row_access_policies.erase(it);
			found = true;
			break;
		}
	}
	if (found) {
		row_access_bindings.erase(std::remove_if(row_access_bindings.begin(), row_access_bindings.end(),
		                                         [&](const QuackapiRowAccessBinding &b) {
			                                         return b.policy_name == name;
		                                         }),
		                          row_access_bindings.end());
	}
	return found;
}

bool QuackapiState::GetRowAccessPolicy(const string &name, QuackapiRowAccessPolicy &out) {
	std::lock_guard<std::mutex> lock(policies_mutex);
	for (auto &p : row_access_policies) {
		if (p.name == name) {
			out = p;
			return true;
		}
	}
	return false;
}

vector<QuackapiRowAccessPolicy> QuackapiState::SnapshotRowAccessPolicies() {
	std::lock_guard<std::mutex> lock(policies_mutex);
	return row_access_policies;
}

void QuackapiState::AddMaskingPolicy(const QuackapiMaskingPolicy &policy, bool or_replace) {
	std::lock_guard<std::mutex> lock(policies_mutex);
	for (auto it = masking_policies.begin(); it != masking_policies.end(); ++it) {
		if (it->name == policy.name) {
			if (!or_replace) {
				throw InvalidInputException(
				    "Masking policy \"%s\" already exists — use CREATE OR REPLACE MASKING POLICY", policy.name);
			}
			*it = policy;
			return;
		}
	}
	for (auto &r : row_access_policies) {
		if (r.name == policy.name) {
			throw InvalidInputException("Policy name \"%s\" is already used by a row access policy", policy.name);
		}
	}
	masking_policies.push_back(policy);
}

bool QuackapiState::DropMaskingPolicy(const string &name) {
	std::lock_guard<std::mutex> lock(policies_mutex);
	bool found = false;
	for (auto it = masking_policies.begin(); it != masking_policies.end(); ++it) {
		if (it->name == name) {
			masking_policies.erase(it);
			found = true;
			break;
		}
	}
	if (found) {
		masking_bindings.erase(std::remove_if(masking_bindings.begin(), masking_bindings.end(),
		                                      [&](const QuackapiMaskingBinding &b) { return b.policy_name == name; }),
		                       masking_bindings.end());
	}
	return found;
}

bool QuackapiState::GetMaskingPolicy(const string &name, QuackapiMaskingPolicy &out) {
	std::lock_guard<std::mutex> lock(policies_mutex);
	for (auto &p : masking_policies) {
		if (p.name == name) {
			out = p;
			return true;
		}
	}
	return false;
}

vector<QuackapiMaskingPolicy> QuackapiState::SnapshotMaskingPolicies() {
	std::lock_guard<std::mutex> lock(policies_mutex);
	return masking_policies;
}

void QuackapiState::BindRowAccessPolicy(const QuackapiRowAccessBinding &binding, bool or_replace) {
	std::lock_guard<std::mutex> lock(policies_mutex);
	const QuackapiRowAccessPolicy *pol = nullptr;
	for (auto &p : row_access_policies) {
		if (p.name == binding.policy_name) {
			pol = &p;
			break;
		}
	}
	if (!pol) {
		throw InvalidInputException("Row access policy \"%s\" does not exist", binding.policy_name);
	}
	if (binding.columns.size() != pol->arg_columns.size()) {
		throw InvalidInputException(
		    "Row access policy \"%s\" expects %llu column(s), got %llu", binding.policy_name,
		    (unsigned long long)pol->arg_columns.size(), (unsigned long long)binding.columns.size());
	}
	// One RAP per table (replace same policy name, or or_replace any).
	for (auto it = row_access_bindings.begin(); it != row_access_bindings.end(); ++it) {
		if (StringUtil::Lower(it->table_name) == StringUtil::Lower(binding.table_name)) {
			if (it->policy_name == binding.policy_name || or_replace) {
				*it = binding;
				return;
			}
			throw InvalidInputException(
			    "Table \"%s\" already has row access policy \"%s\" — DROP it first or use OR REPLACE",
			    binding.table_name, it->policy_name);
		}
	}
	row_access_bindings.push_back(binding);
}

bool QuackapiState::UnbindRowAccessPolicy(const string &table_name, const string &policy_name) {
	std::lock_guard<std::mutex> lock(policies_mutex);
	for (auto it = row_access_bindings.begin(); it != row_access_bindings.end(); ++it) {
		if (StringUtil::Lower(it->table_name) == StringUtil::Lower(table_name) &&
		    (policy_name.empty() || it->policy_name == policy_name)) {
			row_access_bindings.erase(it);
			return true;
		}
	}
	return false;
}

vector<QuackapiRowAccessBinding> QuackapiState::SnapshotRowAccessBindings() {
	std::lock_guard<std::mutex> lock(policies_mutex);
	return row_access_bindings;
}

void QuackapiState::BindMaskingPolicy(const QuackapiMaskingBinding &binding, bool or_replace) {
	std::lock_guard<std::mutex> lock(policies_mutex);
	bool found = false;
	for (auto &p : masking_policies) {
		if (p.name == binding.policy_name) {
			found = true;
			break;
		}
	}
	if (!found) {
		throw InvalidInputException("Masking policy \"%s\" does not exist", binding.policy_name);
	}
	for (auto it = masking_bindings.begin(); it != masking_bindings.end(); ++it) {
		if (StringUtil::Lower(it->table_name) == StringUtil::Lower(binding.table_name) &&
		    StringUtil::Lower(it->column_name) == StringUtil::Lower(binding.column_name)) {
			if (!or_replace && it->policy_name != binding.policy_name) {
				throw InvalidInputException(
				    "Column \"%s\".\"%s\" already has masking policy \"%s\"", binding.table_name, binding.column_name,
				    it->policy_name);
			}
			*it = binding;
			return;
		}
	}
	masking_bindings.push_back(binding);
}

bool QuackapiState::UnbindMaskingPolicy(const string &table_name, const string &column_name) {
	std::lock_guard<std::mutex> lock(policies_mutex);
	for (auto it = masking_bindings.begin(); it != masking_bindings.end(); ++it) {
		if (StringUtil::Lower(it->table_name) == StringUtil::Lower(table_name) &&
		    StringUtil::Lower(it->column_name) == StringUtil::Lower(column_name)) {
			masking_bindings.erase(it);
			return true;
		}
	}
	return false;
}

vector<QuackapiMaskingBinding> QuackapiState::SnapshotMaskingBindings() {
	std::lock_guard<std::mutex> lock(policies_mutex);
	return masking_bindings;
}

} // namespace duckdb
