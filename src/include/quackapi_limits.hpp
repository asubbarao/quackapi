#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

namespace duckdb {
class Connection;
struct QuackapiDeadlineState;

//! Shared watchdog, not a new OS thread for every request. The guard must die
//! before its Connection; teardown synchronizes with any in-flight interrupt.
class QuackapiQueryDeadline {
public:
	QuackapiQueryDeadline(Connection &connection, int64_t timeout_ms);
	~QuackapiQueryDeadline();
	bool Expired() const;
	QuackapiQueryDeadline(const QuackapiQueryDeadline &) = delete;
	QuackapiQueryDeadline &operator=(const QuackapiQueryDeadline &) = delete;

private:
	std::shared_ptr<QuackapiDeadlineState> state;
	std::chrono::steady_clock::time_point previous;
};

//! Outbound clients inherit the request's remaining execution budget.
std::chrono::steady_clock::time_point QuackapiCurrentDeadline();
void QuackapiSetCurrentDeadline(std::chrono::steady_clock::time_point deadline);
int64_t QuackapiRemainingTimeoutMillis(int64_t fallback_ms);
} // namespace duckdb
