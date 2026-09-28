// Standalone regression: compile with -std=c++17 -pthread -Isrc/include.
// Simulate a failure after one worker starts without exhausting host threads.
#include "quackapi_thread_join.hpp"

#include <atomic>
#include <cassert>
#include <stdexcept>
#include <thread>
#include <vector>

int main() {
	std::atomic<int> completed {0};
	try {
		std::vector<std::thread> threads;
		duckdb::JoinOutboundThreads<std::vector<std::thread>> join_threads {threads};
		threads.emplace_back([&completed]() { completed++; });
		throw std::runtime_error("simulated later thread-start failure");
	} catch (const std::runtime_error &) {
		assert(completed == 1);
	}
	{
		std::vector<std::thread> threads;
		duckdb::JoinOutboundThreads<std::vector<std::thread>> join_threads {threads};
		threads.emplace_back([&completed]() { completed++; });
		threads.emplace_back([&completed]() { completed++; });
		threads[0].join(); // Already-joined workers must also be safe.
	}
	assert(completed == 3);
}
