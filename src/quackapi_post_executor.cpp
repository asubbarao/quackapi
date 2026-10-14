#include "quackapi_post_executor.hpp"

#include "quackapi_limits.hpp"
#include "quackapi_thread_join.hpp"

#include <atomic>
#include <thread>

#include "duckdb/common/exception.hpp"

namespace duckdb {

namespace {

constexpr idx_t MAX_POST_FANOUT = 32;

} // namespace

void QuackapiPostExecutor::Execute(idx_t count, const vector<bool> &skip, vector<QuackapiHttpFetchResult> &results,
                                   const QuackapiPostOperation &perform) {
	const auto deadline = QuackapiCurrentDeadline();
	auto run_one = [&](idx_t i) {
		QuackapiSetCurrentDeadline(deadline);
		try {
			results[i] = perform(i);
		} catch (std::exception &e) {
			results[i].request_error = e.what();
		} catch (...) {
			results[i].request_error = "unknown error during outbound HTTP request";
		}
	};

	idx_t active = 0;
	for (idx_t i = 0; i < count; i++) {
		if (!skip[i]) {
			active++;
		}
	}
	if (active == 0) {
		return;
	}
	if (active == 1) {
		for (idx_t i = 0; i < count; i++) {
			if (!skip[i]) {
				run_one(i);
			}
		}
		return;
	}

	const idx_t n_threads = active < MAX_POST_FANOUT ? active : MAX_POST_FANOUT;
	std::atomic<idx_t> cursor {0};
	vector<std::thread> threads;
	threads.reserve(n_threads);
	JoinOutboundThreads<vector<std::thread>> join_threads {threads};
	for (idx_t t = 0; t < n_threads; t++) {
		threads.emplace_back([&]() {
			for (;;) {
				const idx_t i = cursor.fetch_add(1);
				if (i >= count) {
					return;
				}
				if (!skip[i]) {
					run_one(i);
				}
			}
		});
	}
}

} // namespace duckdb
