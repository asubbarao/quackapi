#pragma once

#include <mutex>
#include <thread>
#include <tuple>

#include "duckdb/common/optional_idx.hpp"
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

//! One durable job queue registered via CREATE QUEUE.
//! Jobs live in the shared `quackapi_jobs` table (survives restart in the .db
//! file). Queue options live on the DatabaseInstance (like routes) and must be
//! re-declared after reopen — same lifecycle as CREATE ROUTE.
struct QuackapiQueue {
	string name;
	//! Attempts before a job is moved to status='dead' (dead-letter).
	int32_t max_attempts = 3;
	//! Lease duration in seconds after a successful claim (visibility timeout).
	int32_t visibility_timeout_sec = 30;
	//! Base delay in seconds for exponential nack backoff: base^min(attempts,6).
	int32_t backoff_base_sec = 2;
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
	//! Seam for future shared policy / middleware name. Unused in v1 — do not depend.
	string policy;
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
	//! Validated via the community `json_schema` extension at request time.
	string body_schema;
	//! Group this route joined (empty if none). Pattern/auth already expanded.
	string group_name;
	//! OpenAPI tags CSV (from group inheritance or future per-route tags).
	string tags;
};

//! Row-access policy: predicate over table columns + $claims_* (JWT/auth claims).
//! CREATE ROW ACCESS POLICY name AS (col [type], ...) RETURNS BOOLEAN USING (expr)
struct QuackapiRowAccessPolicy {
	string name;
	//! Argument column names from the AS (...) signature (order preserved).
	vector<string> arg_columns;
	//! Optional type names parallel to arg_columns (may be empty strings).
	vector<string> arg_types;
	//! Boolean SQL expression; may reference arg column names and $claims_*.
	string expression;
};

//! Dynamic data masking policy: rewrite a column value from claims context.
//! CREATE MASKING POLICY name ON <type> USING (expr) — `val` is the column value.
struct QuackapiMaskingPolicy {
	string name;
	string value_type; // e.g. VARCHAR, INTEGER
	//! SQL expression returning the same type; use identifier `val` for the raw value.
	string expression;
};

//! Table binding for a row-access policy (ALTER TABLE … ADD ROW ACCESS POLICY).
struct QuackapiRowAccessBinding {
	string table_name;
	string policy_name;
	//! Table columns mapped onto the policy signature (same arity as policy args).
	vector<string> columns;
};

//! Column binding for a masking policy (ALTER TABLE … SET MASKING POLICY).
struct QuackapiMaskingBinding {
	string table_name;
	string column_name;
	string policy_name;
};

//! Transport for CREATE STREAM. SSE is first-class on cpp-httplib; WebSocket is
//! not available on the bundled httplib (no Upgrade/WS API) — WS is rejected at DDL.
enum class QuackapiStreamTransport : uint8_t {
	SSE = 0,
};

//! One SSE push stream registered via CREATE STREAM.
//! Emits text/event-stream (one event per row). Optional interval re-runs the
//! SELECT for polling/tailing — no separate thread pool (blocks the httplib worker).
struct QuackapiStream {
	string name;
	string method = "GET"; // SSE is GET; WS deferred
	string pattern;        // e.g. /events
	string handler_sql;    // AS <select>; named params bind like routes
	//! 0 = run SELECT once and close the stream. >0 = re-run after each empty/full cycle.
	int64_t interval_ms = 0;
	QuackapiStreamTransport transport = QuackapiStreamTransport::SSE;
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

	//! CREATE [OR REPLACE] ROUTE. Throws on duplicate name unless or_replace.
	void AddRoute(const QuackapiRoute &route, bool or_replace);
	//! DROP ROUTE. Returns false if no such route.
	bool DropRoute(const string &name);
	vector<QuackapiRoute> SnapshotRoutes();

	//! CREATE [OR REPLACE] STREAM. Throws on duplicate name unless or_replace.
	void AddStream(const QuackapiStream &stream, bool or_replace);
	//! DROP STREAM. Returns false if no such stream.
	bool DropStream(const string &name);
	vector<QuackapiStream> SnapshotStreams();

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

	//! CREATE [OR REPLACE] QUEUE. Throws on duplicate name unless or_replace.
	void AddQueue(const QuackapiQueue &queue, bool or_replace);
	//! DROP QUEUE. Returns false if no such queue. Does not delete job rows.
	bool DropQueue(const string &name);
	//! Lookup by name. Returns false if not registered.
	bool GetQueue(const string &name, QuackapiQueue &out);
	vector<QuackapiQueue> SnapshotQueues();

	//! Start serving on host:port. Throws if a server already listens there.
	void StartServer(DatabaseInstance &db, const string &host, int port, const QuackapiServeOptions &opts);
	//! Stop the server on port (any host). Returns false if none.
	bool StopServer(int port);
	//! Stop all servers (used at teardown).
	void StopAllServers();
	//! (host, port, http_client_active) for each running server.
	vector<std::tuple<string, int, string>> ListServers();

	// --- Row access + masking policies (JWT/claims keyed, not DB roles) ---
	void AddRowAccessPolicy(const QuackapiRowAccessPolicy &policy, bool or_replace);
	bool DropRowAccessPolicy(const string &name);
	bool GetRowAccessPolicy(const string &name, QuackapiRowAccessPolicy &out);
	vector<QuackapiRowAccessPolicy> SnapshotRowAccessPolicies();

	void AddMaskingPolicy(const QuackapiMaskingPolicy &policy, bool or_replace);
	bool DropMaskingPolicy(const string &name);
	bool GetMaskingPolicy(const string &name, QuackapiMaskingPolicy &out);
	vector<QuackapiMaskingPolicy> SnapshotMaskingPolicies();

	//! Bind / unbind row-access policy on a table. Throws if policy missing or arity mismatch.
	void BindRowAccessPolicy(const QuackapiRowAccessBinding &binding, bool or_replace);
	bool UnbindRowAccessPolicy(const string &table_name, const string &policy_name);
	vector<QuackapiRowAccessBinding> SnapshotRowAccessBindings();

	void BindMaskingPolicy(const QuackapiMaskingBinding &binding, bool or_replace);
	bool UnbindMaskingPolicy(const string &table_name, const string &column_name);
	vector<QuackapiMaskingBinding> SnapshotMaskingBindings();

private:
	std::mutex routes_mutex;
	vector<QuackapiRoute> routes;

	std::mutex streams_mutex;
	vector<QuackapiStream> streams;

	std::mutex groups_mutex;
	vector<QuackapiGroup> groups;

	std::mutex auths_mutex;
	vector<QuackapiAuth> auths;
	vector<QuackapiApiKeyEntry> api_keys;

	std::mutex queues_mutex;
	vector<QuackapiQueue> queues;

	std::mutex policies_mutex;
	vector<QuackapiRowAccessPolicy> row_access_policies;
	vector<QuackapiMaskingPolicy> masking_policies;
	vector<QuackapiRowAccessBinding> row_access_bindings;
	vector<QuackapiMaskingBinding> masking_bindings;

	std::mutex servers_mutex;
	unordered_map<string, unique_ptr<QuackapiHttpServer>> servers;
};

} // namespace duckdb
