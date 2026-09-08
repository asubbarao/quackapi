#include "quackapi_limits.hpp"
#include "duckdb/main/connection.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>

namespace duckdb {
static thread_local std::chrono::steady_clock::time_point current_deadline =
    std::chrono::steady_clock::time_point::max();

struct QuackapiDeadlineState {
	std::mutex mutex;
	Connection *connection;
	std::chrono::steady_clock::time_point deadline;
	std::atomic<bool> expired {false};
};

class DeadlineWatchdog {
public:
	DeadlineWatchdog() : worker([this]() { Run(); }) {
	}
	~DeadlineWatchdog() {
		{
			std::lock_guard<std::mutex> guard(mutex);
			stopping = true;
		}
		wake.notify_one();
		worker.join();
	}
	void Add(const std::shared_ptr<QuackapiDeadlineState> &state) {
		std::lock_guard<std::mutex> guard(mutex);
		states.erase(std::remove_if(states.begin(), states.end(),
		                            [](const std::weak_ptr<QuackapiDeadlineState> &item) { return item.expired(); }),
		             states.end());
		states.push_back(state);
		wake.notify_one();
	}

private:
	void Run() {
		std::unique_lock<std::mutex> guard(mutex);
		while (!stopping) {
			wake.wait_for(guard, std::chrono::milliseconds(5));
			const auto now = std::chrono::steady_clock::now();
			for (auto it = states.begin(); it != states.end();) {
				auto state = it->lock();
				if (!state) {
					it = states.erase(it);
					continue;
				}
				if (now >= state->deadline) {
					std::lock_guard<std::mutex> state_guard(state->mutex);
					if (state->connection) {
						state->expired = true;
						state->connection->Interrupt();
					}
				}
				++it;
			}
		}
	}
	std::mutex mutex;
	std::condition_variable wake;
	bool stopping = false;
	std::vector<std::weak_ptr<QuackapiDeadlineState>> states;
	std::thread worker;
};

static DeadlineWatchdog &Watchdog() {
	static DeadlineWatchdog watchdog;
	return watchdog;
}

QuackapiQueryDeadline::QuackapiQueryDeadline(Connection &connection, int64_t timeout_ms) : previous(current_deadline) {
	state = std::make_shared<QuackapiDeadlineState>();
	state->connection = &connection;
	state->deadline = std::min(previous, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
	current_deadline = state->deadline;
	Watchdog().Add(state);
}
QuackapiQueryDeadline::~QuackapiQueryDeadline() {
	{
		std::lock_guard<std::mutex> guard(state->mutex);
		state->connection = nullptr;
	}
	current_deadline = previous;
}
bool QuackapiQueryDeadline::Expired() const {
	return state->expired || std::chrono::steady_clock::now() >= state->deadline;
}
std::chrono::steady_clock::time_point QuackapiCurrentDeadline() {
	return current_deadline;
}
void QuackapiSetCurrentDeadline(std::chrono::steady_clock::time_point deadline) {
	current_deadline = deadline;
}
int64_t QuackapiRemainingTimeoutMillis(int64_t fallback_ms) {
	if (current_deadline == std::chrono::steady_clock::time_point::max()) {
		return fallback_ms;
	}
	return std::max<int64_t>(1, std::min(fallback_ms, std::chrono::duration_cast<std::chrono::milliseconds>(
	                                                      current_deadline - std::chrono::steady_clock::now())
	                                                      .count()));
}
} // namespace duckdb
