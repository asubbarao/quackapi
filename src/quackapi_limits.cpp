#include "quackapi_limits.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>

namespace duckdb {
using QuackapiClock = std::chrono::steady_clock;

static thread_local QuackapiClock::time_point current_deadline = QuackapiClock::time_point::max();

//! Per-connection deadline and serving budgets. Read from any thread a query runs on.
struct QuackapiContextBudget : public ClientContextState {
	std::atomic<QuackapiClock::rep> deadline {QuackapiClock::time_point::max().time_since_epoch().count()};
	std::atomic<int64_t> query_timeout_ms {0};
	std::atomic<int64_t> max_response_bytes {0};
	void QueryBegin(ClientContext &context) override {
		auto absolute = deadline.load();
		if (absolute == QuackapiClock::time_point::max().time_since_epoch().count()) {
			return;
		}
		// BeginQueryInternal has already installed max_execution_time. Tighten it
		// to the whole request's absolute deadline, including preceding statements.
		// Round up so native checks cannot expire before the receipt's deadline.
		auto millis =
		    std::chrono::duration_cast<std::chrono::milliseconds>(QuackapiClock::duration(absolute)).count() + 1;
		auto native_deadline = NumericCast<idx_t>(MaxValue<int64_t>(0, millis));
		if (!context.query_deadline.IsValid() || native_deadline < context.query_deadline.GetIndex()) {
			context.query_deadline = native_deadline;
		}
	}
};

static const char *const QUACKAPI_BUDGET_KEY = "quackapi_request_budget";

static shared_ptr<QuackapiContextBudget> BudgetOf(ClientContext &context) {
	return context.registered_state->Get<QuackapiContextBudget>(QUACKAPI_BUDGET_KEY);
}

static shared_ptr<QuackapiContextBudget> BudgetFor(Connection &connection) {
	return connection.context->registered_state->GetOrCreate<QuackapiContextBudget>(QUACKAPI_BUDGET_KEY);
}

struct QuackapiDeadlineState {
	std::mutex mutex;
	Connection *connection;
	ClientContext *parent = nullptr;
	QuackapiClock::time_point deadline;
	std::atomic<bool> expired {false};
	std::atomic<bool> disconnected {false};
	std::function<bool()> disconnect_probe;
	shared_ptr<QuackapiContextBudget> budget;
	QuackapiClock::rep budget_previous = 0;
};

struct QuackapiCancellationState {
	std::mutex mutex;
	ClientContext *context = nullptr;
	std::function<void()> action;
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
	void Add(const std::shared_ptr<QuackapiCancellationState> &state) {
		std::lock_guard<std::mutex> guard(mutex);
		cancellations.erase(
		    std::remove_if(cancellations.begin(), cancellations.end(),
		                   [](const std::weak_ptr<QuackapiCancellationState> &item) { return item.expired(); }),
		    cancellations.end());
		cancellations.push_back(state);
		wake.notify_one();
	}

private:
	void Run() {
		std::unique_lock<std::mutex> guard(mutex);
		while (!stopping) {
			wake.wait_for(guard, std::chrono::milliseconds(5));
			const auto now = QuackapiClock::now();
			for (auto it = states.begin(); it != states.end();) {
				auto state = it->lock();
				if (!state) {
					it = states.erase(it);
					continue;
				}
				std::lock_guard<std::mutex> state_guard(state->mutex);
				if (state->connection) {
					if (now >= state->deadline) {
						state->expired = true;
						state->connection->Interrupt();
					} else if (state->disconnected || (state->disconnect_probe && state->disconnect_probe())) {
						state->disconnected = true;
						state->connection->Interrupt();
					} else if (state->parent && state->parent->IsInterrupted()) {
						state->connection->Interrupt();
					}
				}
				++it;
			}
			for (auto it = cancellations.begin(); it != cancellations.end();) {
				auto state = it->lock();
				if (!state) {
					it = cancellations.erase(it);
					continue;
				}
				std::lock_guard<std::mutex> state_guard(state->mutex);
				if (state->context && state->action && state->context->IsInterrupted()) {
					// Execute once, synchronously with guard teardown. Never let an
					// exception from a transport callback escape the watchdog.
					auto action = std::move(state->action);
					try {
						action();
					} catch (...) {
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
	std::vector<std::weak_ptr<QuackapiCancellationState>> cancellations;
	std::thread worker;
};

static DeadlineWatchdog &Watchdog() {
	static DeadlineWatchdog watchdog;
	return watchdog;
}

QuackapiCancellationGuard::QuackapiCancellationGuard(ClientContext &context, std::function<void()> action)
    : state(std::make_shared<QuackapiCancellationState>()) {
	state->context = &context;
	state->action = std::move(action);
	Watchdog().Add(state);
}

QuackapiCancellationGuard::~QuackapiCancellationGuard() {
	std::lock_guard<std::mutex> guard(state->mutex);
	state->context = nullptr;
	state->action = nullptr;
}

QuackapiQueryDeadline::QuackapiQueryDeadline(Connection &connection, int64_t timeout_ms) : previous(current_deadline) {
	Start(connection, std::min(previous, QuackapiClock::now() + std::chrono::milliseconds(timeout_ms)), nullptr);
}
QuackapiQueryDeadline::QuackapiQueryDeadline(Connection &connection, QuackapiClock::time_point deadline,
                                             ClientContext *parent, bool thread_affine_p)
    : previous(current_deadline), thread_affine(thread_affine_p) {
	Start(connection, deadline, parent);
}
void QuackapiQueryDeadline::Start(Connection &connection, QuackapiClock::time_point deadline, ClientContext *parent) {
	state = std::make_shared<QuackapiDeadlineState>();
	state->connection = &connection;
	state->parent = parent;
	state->deadline = deadline;
	state->budget = BudgetFor(connection);
	state->budget_previous = state->budget->deadline.exchange(deadline.time_since_epoch().count());
	if (thread_affine) {
		current_deadline = state->deadline;
	}
	Watchdog().Add(state);
}
QuackapiQueryDeadline::~QuackapiQueryDeadline() {
	{
		std::lock_guard<std::mutex> guard(state->mutex);
		state->connection = nullptr;
		state->parent = nullptr;
		state->disconnect_probe = nullptr;
	}
	// Restore only if no later guard on this connection replaced the deadline.
	auto ours = state->deadline.time_since_epoch().count();
	state->budget->deadline.compare_exchange_strong(ours, state->budget_previous);
	if (thread_affine) {
		current_deadline = previous;
	}
}
bool QuackapiQueryDeadline::Expired() const {
	return state->expired || QuackapiClock::now() >= state->deadline;
}
void QuackapiQueryDeadline::WatchDisconnect(std::function<bool()> probe) {
	std::lock_guard<std::mutex> guard(state->mutex);
	state->disconnect_probe = std::move(probe);
}
bool QuackapiQueryDeadline::Disconnected() const {
	return state->disconnected;
}
QuackapiClock::time_point QuackapiCurrentDeadline() {
	return current_deadline;
}
void QuackapiSetCurrentDeadline(QuackapiClock::time_point deadline) {
	current_deadline = deadline;
}
int64_t QuackapiRemainingTimeoutMillis(int64_t fallback_ms) {
	if (current_deadline == QuackapiClock::time_point::max()) {
		return fallback_ms;
	}
	return std::max<int64_t>(
	    1, std::min(
	           fallback_ms,
	           std::chrono::duration_cast<std::chrono::milliseconds>(current_deadline - QuackapiClock::now()).count()));
}
QuackapiClock::time_point QuackapiContextDeadline(ClientContext &context) {
	auto deadline = current_deadline;
	auto budget = BudgetOf(context);
	if (budget) {
		deadline = std::min(deadline, QuackapiClock::time_point(QuackapiClock::duration(budget->deadline.load())));
	}
	if (context.query_deadline.IsValid()) {
		// Native max_execution_time is a deadline, not an interrupt flag. Carry
		// it into nested connections and outbound I/O before a scalar can block
		// the caller's next native interrupt check. Compare in milliseconds first
		// so an extremely distant native deadline cannot overflow clock duration.
		auto deadline_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline.time_since_epoch()).count();
		if (deadline_ms >= 0 && context.query_deadline.GetIndex() <= NumericCast<idx_t>(deadline_ms)) {
			deadline = QuackapiClock::time_point(std::chrono::duration_cast<QuackapiClock::duration>(
			    std::chrono::milliseconds(NumericCast<int64_t>(context.query_deadline.GetIndex()))));
		}
	}
	return deadline;
}
void QuackapiSetServingBudget(Connection &connection, int64_t query_timeout_ms, int64_t max_response_bytes) {
	auto budget = BudgetFor(connection);
	budget->query_timeout_ms = query_timeout_ms;
	budget->max_response_bytes = max_response_bytes;
}
bool QuackapiGetServingBudget(ClientContext &context, int64_t &query_timeout_ms, int64_t &max_response_bytes) {
	auto budget = BudgetOf(context);
	if (!budget || budget->query_timeout_ms <= 0) {
		return false;
	}
	query_timeout_ms = budget->query_timeout_ms;
	max_response_bytes = budget->max_response_bytes;
	return true;
}
} // namespace duckdb
