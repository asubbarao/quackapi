#pragma once

namespace duckdb {

// Join before captured state is destroyed, including when a later thread fails
// to start. Destroying an already-joinable std::thread would terminate the host.
template <class THREADS>
struct JoinOutboundThreads {
	THREADS &threads;
	~JoinOutboundThreads() {
		for (auto &thread : threads) {
			if (thread.joinable()) {
				thread.join();
			}
		}
	}
};

} // namespace duckdb
