//===----------------------------------------------------------------------===//
// quackapi_post_executor.hpp
//
// Database-owned bounded execution for outbound POST requests.
//===----------------------------------------------------------------------===//
#pragma once

#include "quackapi_http_fetch.hpp"
#include "duckdb/storage/object_cache.hpp"

#include <atomic>
#include <memory>

namespace duckdb {

struct QuackapiPostWorkerState {
	virtual ~QuackapiPostWorkerState() = default;
};

struct QuackapiPostRequest {
	string url;
	string body;
	string content_type;
	unordered_map<string, string> headers;
	shared_ptr<std::atomic<bool>> cancelled;
};

class ClientContext;
class DatabaseInstance;

class QuackapiPostExecutor : public ObjectCacheEntry {
public:
	explicit QuackapiPostExecutor(DatabaseInstance &db);
	~QuackapiPostExecutor() override;
	string GetObjectType() override;
	optional_idx GetEstimatedCacheMemory() const override;
	static vector<QuackapiHttpFetchResult> Execute(ClientContext &context, vector<QuackapiPostRequest> requests,
	                                               const vector<bool> &skip);
	static string ObjectType();

private:
	class Impl;
	unique_ptr<Impl> impl;
};

} // namespace duckdb
