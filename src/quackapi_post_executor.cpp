#include "quackapi_post_executor.hpp"

#include "quackapi_limits.hpp"
#include "quackapi_post_provider.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace duckdb {

namespace {

constexpr auto POST_CONCURRENCY_SETTING = "quackapi_post_concurrency";

struct PostBatch {
	vector<QuackapiHttpFetchResult> results;
	vector<shared_ptr<std::atomic<bool>>> cancellation_flags;
	idx_t completed = 0;
	idx_t accepted = 0;
	std::atomic<bool> cancelled {false};
	std::atomic<bool> interrupted {false};
	std::chrono::steady_clock::time_point deadline;
	std::mutex mutex;
	std::condition_variable cv;
};

struct PostJob {
	shared_ptr<PostBatch> batch;
	idx_t row;
	QuackapiPostRequest request;
};

bool IsExpired(std::chrono::steady_clock::time_point deadline) {
	return deadline != std::chrono::steady_clock::time_point::max() && std::chrono::steady_clock::now() >= deadline;
}

void CheckCaller(ClientContext &context, std::chrono::steady_clock::time_point deadline) {
	if (context.IsInterrupted() || IsExpired(deadline)) {
		throw InterruptException();
	}
}

idx_t EffectiveConcurrency(DatabaseInstance &db) {
	Value setting;
	auto found = db.TryGetCurrentSetting(POST_CONCURRENCY_SETTING, setting);
	if (found) {
		const auto configured = setting.GetValue<int64_t>();
		if (configured > 0) {
			return NumericCast<idx_t>(configured);
		}
	}
	return MaxValue<idx_t>(1, db.NumberOfThreads());
}

} // namespace

class QuackapiPostExecutor::Impl {
public:
	explicit Impl(DatabaseInstance &db_p) : db(db_p) {
	}

	~Impl() {
		{
			std::lock_guard<std::mutex> guard(lock);
			stopping = true;
		}
		queue_cv.notify_all();
		space_cv.notify_all();
		for (auto &worker : workers) {
			if (worker.joinable()) {
				worker.join();
			}
		}
	}

	vector<QuackapiHttpFetchResult> Execute(ClientContext &context, vector<QuackapiPostRequest> requests,
	                                        const vector<bool> &skip) {
		const auto deadline = QuackapiCurrentDeadline();
		auto batch = make_shared_ptr<PostBatch>();
		batch->results.resize(requests.size());
		batch->cancellation_flags.resize(requests.size());
		batch->deadline = deadline;
		for (idx_t i = 0; i < requests.size(); i++) {
			if (!skip[i]) {
				requests[i].cancelled = make_shared_ptr<std::atomic<bool>>(false);
				batch->cancellation_flags[i] = requests[i].cancelled;
			}
		}
		const auto concurrency = EffectiveConcurrency(db);
		const auto queue_capacity =
		    concurrency > NumericLimits<idx_t>::Maximum() / 2 ? NumericLimits<idx_t>::Maximum() : concurrency * 2;

		try {
			for (idx_t i = 0; i < requests.size(); i++) {
				if (skip[i]) {
					continue;
				}
				CheckCaller(context, deadline);
				auto job = make_shared_ptr<PostJob>();
				job->batch = batch;
				job->row = i;
				job->request = std::move(requests[i]);

				std::unique_lock<std::mutex> guard(lock);
				while (!stopping && queue.size() >= queue_capacity) {
					CheckCaller(context, deadline);
					space_cv.wait_for(guard, std::chrono::milliseconds(10));
				}
				CheckCaller(context, deadline);
				if (stopping || batch->cancelled.load()) {
					throw InterruptException();
				}
				EnsureWorkers(concurrency);
				queue.push_back(std::move(job));
				{
					std::lock_guard<std::mutex> batch_guard(batch->mutex);
					batch->accepted++;
				}
				queue_cv.notify_all();
			}
		} catch (InterruptException &) {
			CancelBatch(batch);
			DrainBatch(batch, Accepted(batch));
			throw;
		}

		const auto expected = requests.size() - CountSkipped(skip);
		while (true) {
			{
				std::lock_guard<std::mutex> guard(batch->mutex);
				if (batch->completed >= expected) {
					break;
				}
			}
			if (context.IsInterrupted() || IsExpired(deadline) || batch->interrupted.load()) {
				CancelBatch(batch);
			}
			std::unique_lock<std::mutex> guard(batch->mutex);
			batch->cv.wait_for(guard, std::chrono::milliseconds(10));
		}
		if (batch->interrupted.load() || context.IsInterrupted() || IsExpired(deadline)) {
			throw InterruptException();
		}
		return std::move(batch->results);
	}

private:
	static idx_t Accepted(const shared_ptr<PostBatch> &batch) {
		std::lock_guard<std::mutex> guard(batch->mutex);
		return batch->accepted;
	}

	static idx_t CountSkipped(const vector<bool> &skip) {
		idx_t count = 0;
		for (auto value : skip) {
			count += value ? 1 : 0;
		}
		return count;
	}

	void EnsureWorkers(idx_t desired) {
		while (workers.size() < desired) {
			workers.emplace_back([this]() { WorkerLoop(); });
		}
	}

	void CancelQueued(const shared_ptr<PostBatch> &batch) {
		std::lock_guard<std::mutex> guard(lock);
		for (auto it = queue.begin(); it != queue.end();) {
			if ((*it)->batch == batch) {
				(*it)->request.cancelled->store(true);
				{
					std::lock_guard<std::mutex> batch_guard(batch->mutex);
					batch->completed++;
				}
				it = queue.erase(it);
			} else {
				++it;
			}
		}
		space_cv.notify_all();
		batch->cv.notify_all();
	}

	void CancelBatch(const shared_ptr<PostBatch> &batch) {
		batch->cancelled.store(true);
		for (const auto &flag : batch->cancellation_flags) {
			if (flag) {
				flag->store(true);
			}
		}
		CancelQueued(batch);
	}

	void DrainBatch(const shared_ptr<PostBatch> &batch, idx_t expected) {
		std::unique_lock<std::mutex> guard(batch->mutex);
		batch->cv.wait(guard, [&]() { return batch->completed >= expected; });
	}

	void WorkerLoop() {
		unique_ptr<QuackapiPostWorkerState> state;
		try {
			state = QuackapiPostProvider::CreateWorkerState(db);
		} catch (...) {
			// Each task receives a concrete error below if worker state setup failed.
		}
		for (;;) {
			shared_ptr<PostJob> job;
			{
				std::unique_lock<std::mutex> guard(lock);
				queue_cv.wait(guard,
				              [&]() { return stopping || (!queue.empty() && active < EffectiveConcurrency(db)); });
				if (stopping && queue.empty()) {
					break;
				}
				job = std::move(queue.front());
				queue.pop_front();
				active++;
				space_cv.notify_all();
			}

			auto &batch = *job->batch;
			bool skip_job;
			{
				std::lock_guard<std::mutex> guard(batch.mutex);
				skip_job = batch.cancelled.load() || job->request.cancelled->load() || IsExpired(batch.deadline);
			}
			QuackapiHttpFetchResult response;
			if (!skip_job) {
				const auto old_deadline = QuackapiCurrentDeadline();
				QuackapiSetCurrentDeadline(batch.deadline);
				try {
					if (!state) {
						throw IOException("failed to initialize outbound POST worker");
					}
					if (job->request.cancelled->load() || IsExpired(batch.deadline)) {
						throw InterruptException();
					}
					response = QuackapiPostProvider::Post(db, *state, job->request);
				} catch (InterruptException &) {
					batch.cancelled.store(true);
					batch.interrupted.store(true);
				} catch (std::exception &e) {
					response.request_error = e.what();
				} catch (...) {
					response.request_error = "unknown error during outbound HTTP request";
				}
				QuackapiSetCurrentDeadline(old_deadline);
			}
			{
				std::lock_guard<std::mutex> guard(batch.mutex);
				if (!skip_job && !batch.cancelled.load()) {
					batch.results[job->row] = std::move(response);
				}
				batch.completed++;
			}
			{
				std::lock_guard<std::mutex> guard(lock);
				active--;
			}
			queue_cv.notify_all();
			batch.cv.notify_all();
			if (batch.cancelled.load()) {
				CancelQueued(job->batch);
			}
		}
		state.reset();
	}

	DatabaseInstance &db;
	std::mutex lock;
	std::condition_variable queue_cv;
	std::condition_variable space_cv;
	std::deque<shared_ptr<PostJob>> queue;
	vector<std::thread> workers;
	idx_t active = 0;
	bool stopping = false;
};

QuackapiPostExecutor::QuackapiPostExecutor(DatabaseInstance &db) : impl(make_uniq<Impl>(db)) {
}

QuackapiPostExecutor::~QuackapiPostExecutor() = default;

string QuackapiPostExecutor::ObjectType() {
	return "quackapi_post_executor";
}

string QuackapiPostExecutor::GetObjectType() {
	return ObjectType();
}

optional_idx QuackapiPostExecutor::GetEstimatedCacheMemory() const {
	return optional_idx();
}

vector<QuackapiHttpFetchResult>
QuackapiPostExecutor::Execute(ClientContext &context, vector<QuackapiPostRequest> requests, const vector<bool> &skip) {
	auto &db = DatabaseInstance::GetDatabase(context);
	auto executor = db.GetObjectCache().GetOrCreateWithTypePrefix<QuackapiPostExecutor>("database", db);
	return executor->impl->Execute(context, std::move(requests), skip);
}

} // namespace duckdb
