#pragma once

#include <mutex>
#include <thread>

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace duckdb {

class DatabaseInstance;
class QuackapiHttpServer;

//! One registered route. Immutable once snapshotted; the registry replaces
//! entries wholesale on CREATE OR REPLACE ROUTE.
struct QuackapiRoute {
	string name;
	string method;      // upper-case: GET / POST / PUT / DELETE / PATCH / HEAD
	string pattern;     // e.g. /items/:id — ':name' and '{name}' segments capture path params
	string handler_sql; // the AS <select> body; named parameters ($id) bind request params
	int status = 200;   // success status code
};

//! Per-database quackapi state: the route registry and running servers.
//! Lives in the DatabaseInstance's ObjectCache (non-evictable), so LOAD never
//! touches the user's catalog and state dies with the database.
class QuackapiState : public ObjectCacheEntry {
public:
	static constexpr const char *CACHE_KEY = "quackapi_state";

	~QuackapiState() override;

	static string ObjectType() {
		return "quackapi_state";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		// Invalid index marks the entry non-evictable — the registry must never
		// be dropped by cache pressure.
		return optional_idx();
	}

	static QuackapiState &Get(DatabaseInstance &db);

	//! CREATE [OR REPLACE] ROUTE. Throws on duplicate name unless or_replace.
	void AddRoute(const QuackapiRoute &route, bool or_replace);
	//! DROP ROUTE. Returns false if no such route.
	bool DropRoute(const string &name);
	vector<QuackapiRoute> SnapshotRoutes();

	//! Start serving on host:port. Throws if a server already listens there.
	void StartServer(DatabaseInstance &db, const string &host, int port);
	//! Stop the server on port (any host). Returns false if none.
	bool StopServer(int port);
	//! Stop all servers (used at teardown).
	void StopAllServers();
	vector<std::pair<string, int>> ListServers();

private:
	std::mutex routes_mutex;
	vector<QuackapiRoute> routes;

	std::mutex servers_mutex;
	unordered_map<string, unique_ptr<QuackapiHttpServer>> servers;
};

} // namespace duckdb
