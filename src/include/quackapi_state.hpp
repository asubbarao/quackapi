#pragma once

#include <atomic>
#include <mutex>
#include <thread>

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/storage/object_cache.hpp"

#include "quackapi_server.hpp"

namespace duckdb {

class DatabaseInstance;

//! Auth scheme kind registered via CREATE AUTH.
enum class QuackapiAuthKind : uint8_t {
	API_KEY = 0,
	JWT_HS256 = 1,
};

//! One registered auth scheme. Secret is never exposed via table functions.
struct QuackapiAuth {
	string name;
	QuackapiAuthKind kind = QuackapiAuthKind::API_KEY;
	//! For API_KEY: header to read (default "X-API-Key"). Authorization: Bearer
	//! is always accepted as a fallback for API_KEY.
	string header = "X-API-Key";
	//! For JWT_HS256: HMAC secret. Empty for API_KEY.
	string secret;
};

//! One stored API key (raw key is never retained — only SHA-256 hash).
struct QuackapiApiKeyEntry {
	string auth_name;
	string key_hash; // 32 raw SHA-256 bytes
	string subject;
};

//! Where a route param is bound from (FastAPI Query / Header / Cookie).
enum class QuackapiParamSource : uint8_t {
	QUERY = 0,  // default: query string (+ path captures still win by pattern)
	HEADER = 1, // request header (name: external_name or underscore→hyphen of name)
	COOKIE = 2, // Cookie header field (name: external_name or param name)
};

//! Optional/constraint metadata for a route parameter (FastAPI Query/Header/Cookie parity).
//! Declared via CREATE ROUTE … PARAM <name> [TYPE] [HEADER|COOKIE [name]] [DEFAULT …] [GE/…]
struct QuackapiParamSpec {
	string name;
	//! Optional type hint (INTEGER, BIGINT, VARCHAR, …). Empty = infer from SQL.
	string type_name;
	QuackapiParamSource source = QuackapiParamSource::QUERY;
	//! Wire name for HEADER/COOKIE when different from param name. Empty = derive:
	//! HEADER → name with '_' → '-'; COOKIE → param name as-is.
	string external_name;
	bool has_default = false;
	bool default_is_null = false;
	//! Raw default literal (digits / true / false / unquoted text). Empty when default_is_null.
	string default_raw;
	bool has_ge = false;
	double ge = 0;
	bool has_gt = false;
	double gt = 0;
	bool has_le = false;
	double le = 0;
	bool has_lt = false;
	double lt = 0;
	bool has_min_length = false;
	idx_t min_length = 0;
	bool has_max_length = false;
	idx_t max_length = 0;
};

//! One registered route group (FastAPI APIRouter-style prefix + default auth).
//! Expanded into routes at CREATE ROUTE time — runtime registry stays flat.
struct QuackapiGroup {
	string name;
	//! Absolute URL prefix (must start with /). Joined onto route paths at CREATE.
	string prefix;
	//! Default auth scheme name. Empty = public. Inherited when route omits REQUIRE.
	string require_auth;
	//! OpenAPI tags CSV (e.g. "items,v1"). Inherited by routes that join the group.
	string tags;
};

//! One registered route. Immutable once snapshotted; the registry replaces
//! entries wholesale on CREATE OR REPLACE ROUTE.
struct QuackapiRoute {
	string name;
	string method;      // upper-case: GET / POST / PUT / DELETE / PATCH / HEAD
	string pattern;     // e.g. /items/:id — ':name' and '{name}' segments capture path params
	string handler_sql; // the AS <select> body; named parameters ($id) bind request params
	int status = 200;   // success status code
	//! Empty = public. Non-empty = name of a CREATE AUTH scheme required to call.
	string require_auth;
	//! PARAM specs: optional defaults + FastAPI-style numeric/string constraints.
	vector<QuackapiParamSpec> params;
	//! Optional JSON Schema (draft) for the request body. Empty = no schema check.
	//! The community `json_schema` extension is resolved when the route is created;
	//! request time only loads the already-installed extension.
	string body_schema;
	//! Optional DuckDB JSON transform structure for a typed request body. The
	//! structure uses native DuckDB names (e.g. {"items":[{"id":"INTEGER"}]}),
	//! is checked by json_transform_strict, and lets $body bind as STRUCT/LIST
	//! without a duplicate application model. Empty = no typed body transform.
	string body_type;
	//! Group this route joined (empty if none). Pattern/auth already expanded.
	string group_name;
	//! OpenAPI tags CSV (from group inheritance or future per-route tags).
	string tags;
	//! RATE LIMIT <n> PER <seconds> [BY ip|token|key]. n==0 disables.
	int rate_limit_n = 0;
	int rate_limit_per_sec = 0;
	//! "ip" (default), "token" (Authorization / X-API-Key header raw), "key" alias of token.
	string rate_limit_by;
	//! Default response body format: "json" (default), "ndjson", or "csv".
	//! Explicit non-json wins over Accept negotiation; json allows Accept override.
	string response_format = "json";
	//! JSON shape for FORMAT json: "array" (default, `[{…}]`) or "object" (single row → `{…}`).
	//! Object requires exactly one row; >1 rows → 500. Only valid with FORMAT json.
	string response_envelope = "array";
	//! When handler returns 0 rows and this is set (≠0), respond with this status instead of
	//! the success STATUS + empty array/null. 0 = unset (default empty-result behavior).
	int empty_status = 0;
	//! Body for EMPTY STATUS responses. Empty string → `{"detail":"Not Found"}`.
	string empty_body;
	//! Per-request httplib socket read/write timeout in seconds. 0 = use serve
	//! defaults (QUACKAPI_DEFAULT_IO_TIMEOUT_SEC / quackapi_serve read/write_timeout_sec).
	//! When set, HandleRequest extends SO_RCVTIMEO/SO_SNDTIMEO and the active
	//! SocketStream select deadlines for this connection only.
	int32_t timeout_sec = 0;
	//! Optional six-field cron expression (seconds first), owned by cronjob.
	//! Empty means this route is not scheduled.
	string schedule;
};

//! Per-database quackapi state: the route registry, auth registry, and running servers.
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
	//! Database-scoped quotas; raw credentials are never retained as keys.
	bool AllowRateLimit(const string &route, const string &client, int limit, int per_sec, int &retry_after);
	void ClearRouteRateLimit(const string &route);

	//! CREATE [OR REPLACE] ROUTE. Throws on duplicate name unless or_replace.
	void AddRoute(const QuackapiRoute &route, bool or_replace);
	//! DROP ROUTE. Returns false if no such route.
	bool DropRoute(const string &name);
	vector<QuackapiRoute> SnapshotRoutes();
	//! Shared immutable view for the HTTP hot path (refcount only — no deep copy).
	shared_ptr<const vector<QuackapiRoute>> LiveRoutes();

	//! CREATE [OR REPLACE] GROUP. Throws on duplicate name unless or_replace.
	void AddGroup(const QuackapiGroup &group, bool or_replace);
	//! DROP GROUP. Does NOT cascade-drop member routes. Returns false if missing.
	bool DropGroup(const string &name);
	//! Lookup by name. Returns false if not registered.
	bool GetGroup(const string &name, QuackapiGroup &out);
	vector<QuackapiGroup> SnapshotGroups();

	//! CREATE [OR REPLACE] AUTH. Throws on duplicate name unless or_replace.
	void AddAuth(const QuackapiAuth &auth, bool or_replace);
	//! DROP AUTH. Also removes API keys bound to that auth name. Returns false if missing.
	bool DropAuth(const string &name);
	//! Lookup by name. Returns false if not registered.
	bool GetAuth(const string &name, QuackapiAuth &out);
	vector<QuackapiAuth> SnapshotAuths();

	//! Store a hashed API key for an API_KEY auth. Throws if auth missing / wrong kind.
	//! Returns the subject on success.
	string AddApiKey(const string &auth_name, const string &raw_key, const string &subject);
	//! Snapshot of stored keys for an auth (hashes only — never raw keys).
	vector<QuackapiApiKeyEntry> SnapshotApiKeys(const string &auth_name);

	//! Start serving on host:port. Throws if a server already listens there.
	void StartServer(DatabaseInstance &db, const string &host, int port, const QuackapiServeOptions &opts);
	//! Stop the server on port (any host). Returns false if none.
	bool StopServer(int port);
	//! Stop all servers (used at teardown).
	void StopAllServers();
	//! True when a quackapi server is registered on this port (any host).
	bool HasServerOnPort(int port);
	//! Host for the server on port, if any. Returns false if none.
	bool GetServerHost(int port, string &host_out);
	//! (host, port) for each running server.
	vector<std::pair<string, int>> ListServers();
	//! Snapshot one server's request ring; port 0 means the only running server.
	//! A port is required when more than one server is running.
	vector<QuackapiRequestRecord> SnapshotRequests(int port = 0);
	//! Snapshot telemetry sinks; port 0 means the only running server.
	//! A port is required when more than one server is running.
	vector<QuackapiTelemetryStatus> SnapshotTelemetryStatus(int port = 0);
	//! Flush queued telemetry for one running server. Port 0 means the only server.
	//! A port is required when more than one server is running.
	bool FlushTelemetry(int port = 0);

private:
	void PublishRoutes();

	struct RateLimitEntry {
		int count = 0;
		std::chrono::steady_clock::time_point expires;
	};
	std::mutex rate_limit_mutex;
	unordered_map<string, RateLimitEntry> rate_limit_entries;

	std::mutex routes_mutex;
	vector<QuackapiRoute> routes;
	//! Published under routes_mutex; readers take a shared_ptr copy and drop the lock.
	shared_ptr<const vector<QuackapiRoute>> routes_live;

	std::mutex groups_mutex;
	vector<QuackapiGroup> groups;

	std::mutex auths_mutex;
	vector<QuackapiAuth> auths;
	vector<QuackapiApiKeyEntry> api_keys;

	std::mutex servers_mutex;
	unordered_map<string, unique_ptr<QuackapiHttpServer>> servers;
};

} // namespace duckdb
