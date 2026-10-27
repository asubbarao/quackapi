#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <functional>

namespace duckdb {
class ClientContext;
class Connection;
struct QuackapiDeadlineState;
struct QuackapiCancellationState;

//! Calls the action when a query is interrupted. Destroy before the context or
//! action's resources; teardown waits for any in-flight action to finish.
class QuackapiCancellationGuard {
public:
	QuackapiCancellationGuard(ClientContext &context, std::function<void()> action);
	~QuackapiCancellationGuard();
	QuackapiCancellationGuard(const QuackapiCancellationGuard &) = delete;
	QuackapiCancellationGuard &operator=(const QuackapiCancellationGuard &) = delete;

private:
	std::shared_ptr<QuackapiCancellationState> state;
};

//! Shared watchdog, not a new OS thread for every request. The guard must die
//! before its Connection; teardown synchronizes with any in-flight interrupt.
//! The deadline is also recorded on the connection, so outbound calls its query
//! makes on DuckDB worker threads (which never see this thread's deadline) honor it.
class QuackapiQueryDeadline {
public:
	QuackapiQueryDeadline(Connection &connection, int64_t timeout_ms);
	//! An absolute deadline. When `parent` is interrupted, so is `connection`;
	//! `parent` must outlive this guard.
	QuackapiQueryDeadline(Connection &connection, std::chrono::steady_clock::time_point deadline, ClientContext *parent,
	                      bool thread_affine = true);
	~QuackapiQueryDeadline();
	bool Expired() const;
	//! Probe is polled by the shared watchdog while this guard is alive.
	void WatchDisconnect(std::function<bool()> probe);
	bool Disconnected() const;
	QuackapiQueryDeadline(const QuackapiQueryDeadline &) = delete;
	QuackapiQueryDeadline &operator=(const QuackapiQueryDeadline &) = delete;

private:
	void Start(Connection &connection, std::chrono::steady_clock::time_point deadline, ClientContext *parent);
	std::shared_ptr<QuackapiDeadlineState> state;
	std::chrono::steady_clock::time_point previous;
	bool thread_affine = true;
};

//! Outbound clients inherit the request's remaining execution budget.
std::chrono::steady_clock::time_point QuackapiCurrentDeadline();
void QuackapiSetCurrentDeadline(std::chrono::steady_clock::time_point deadline);
int64_t QuackapiRemainingTimeoutMillis(int64_t fallback_ms);
//! The earliest thread, connection-budget, or native active-query deadline.
std::chrono::steady_clock::time_point QuackapiContextDeadline(ClientContext &context);

//! A served request's own query_timeout_ms / max_response_bytes, recorded on its
//! connection so quackapi_selfdispatch in the request's SQL applies the server's options.
void QuackapiSetServingBudget(Connection &connection, int64_t query_timeout_ms, int64_t max_response_bytes);
//! False outside a served request (and outside a quackapi_selfdispatch row started inside one).
bool QuackapiGetServingBudget(ClientContext &context, int64_t &query_timeout_ms, int64_t &max_response_bytes);
} // namespace duckdb
