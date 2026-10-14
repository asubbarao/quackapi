#pragma once

#include <algorithm>
#include <string>

inline bool MatchesPinnedDuckDBIdentity(const std::string &library_version, const std::string &source_id) {
	static const std::string pinned_commit = "069cc9f9b5be802405797faecc284961b07c70ef";
	return library_version == "v1.5.6" && (source_id.size() == 8 || source_id.size() == 10) &&
	       std::equal(source_id.begin(), source_id.end(), pinned_commit.begin());
}
