#include "quackapi_limits.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <unordered_map>

namespace duckdb {
static thread_local std::chrono::steady_clock::time_point current_deadline =
    std::chrono::steady_clock::time_point::max();

struct QuackapiDeadlineState {
	std::mutex mutex;
	Connection *connection;
	std::chrono::steady_clock::time_point deadline;
	std::atomic<bool> expired {false};
};

static std::mutex deadline_registry_mutex;
static std::unordered_map<ClientContext *, std::vector<std::weak_ptr<QuackapiDeadlineState>>> deadline_registry;

static void RegisterDeadline(ClientContext *context, const std::shared_ptr<QuackapiDeadlineState> &state) {
	if (!context) {
		return;
	}
	std::lock_guard<std::mutex> guard(deadline_registry_mutex);
	auto &stack = deadline_registry[context];
	stack.erase(std::remove_if(stack.begin(), stack.end(), [](const auto &entry) { return entry.expired(); }),
	            stack.end());
	stack.push_back(state);
}

static void UnregisterDeadline(ClientContext *context, const std::shared_ptr<QuackapiDeadlineState> &state) {
	if (!context) {
		return;
	}
	std::lock_guard<std::mutex> guard(deadline_registry_mutex);
	auto found = deadline_registry.find(context);
	if (found == deadline_registry.end()) {
		return;
	}
	auto &stack = found->second;
	stack.erase(std::remove_if(stack.begin(), stack.end(),
	                           [&](const auto &entry) { return entry.expired() || entry.lock() == state; }),
	            stack.end());
	if (stack.empty()) {
		deadline_registry.erase(found);
	}
}

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
	const auto context_deadline = connection.context ? QuackapiCurrentDeadline(*connection.context)
	                                                 : std::chrono::steady_clock::time_point::max();
	state->deadline = std::min(std::min(previous, context_deadline),
	                           std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
	current_deadline = state->deadline;
	RegisterDeadline(connection.context.get(), state);
	Watchdog().Add(state);
}
QuackapiQueryDeadline::~QuackapiQueryDeadline() {
	UnregisterDeadline(connection.context.get(), state);
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
std::chrono::steady_clock::time_point QuackapiCurrentDeadline(ClientContext &context) {
	std::lock_guard<std::mutex> guard(deadline_registry_mutex);
	auto found = deadline_registry.find(&context);
	if (found == deadline_registry.end()) {
		return std::chrono::steady_clock::time_point::max();
	}
	auto &stack = found->second;
	while (!stack.empty()) {
		auto state = stack.back().lock();
		if (state) {
			return state->deadline;
		}
		stack.pop_back();
	}
	deadline_registry.erase(found);
	return std::chrono::steady_clock::time_point::max();
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
