#include "quackapi_post_provider.hpp"

namespace duckdb {

QuackapiHttpFetchResult QuackapiPostProvider::Post(DatabaseInstance &db, const string &url, const string &body,
                                                   const string &content_type,
                                                   const unordered_map<string, string> &extra_headers) {
	return QuackapiHttpFetch::Post(db, url, body, content_type, extra_headers);
}

} // namespace duckdb
