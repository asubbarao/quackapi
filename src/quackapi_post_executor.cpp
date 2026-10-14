#include "quackapi_post_executor.hpp"

#include "quackapi_limits.hpp"
#include "quackapi_post_provider.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
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

idx_t WaitingQueueCapacity(idx_t concurrency) {
	return concurrency > NumericLimits<idx_t>::Maximum() / 2 ? NumericLimits<idx_t>::Maximum() : concurrency * 2;
}

} // namespace

class QuackapiPostExecutor::Impl {
public:
	struct Worker {
		std::thread thread;
		shared_ptr<PostJob> active_job;
	};

	explicit Impl(DatabaseInstance &db_p) : db(db_p) {
	}

	~Impl() {
		{
			std::lock_guard<std::mutex> guard(lock);
			stopping = true;
			for (auto &job : queue) {
				job->request.cancelled->store(true);
				job->batch->cancelled.store(true);
				job->batch->interrupted.store(true);
				std::lock_guard<std::mutex> batch_guard(job->batch->mutex);
				job->batch->completed++;
				job->batch->cv.notify_all();
			}
			queue.clear();
			for (auto &worker : workers) {
				if (worker->active_job) {
					auto &job = *worker->active_job;
					job.request.cancelled->store(true);
					job.batch->cancelled.store(true);
					job.batch->interrupted.store(true);
					job.batch->cv.notify_all();
				}
			}
		}
		queue_cv.notify_all();
		space_cv.notify_all();
		for (auto &worker : workers) {
			if (worker->thread.joinable()) {
				worker->thread.join();
			}
		}
	}

	vector<QuackapiHttpFetchResult> Execute(ClientContext &context, vector<QuackapiPostRequest> requests,
	                                        const vector<bool> &skip) {
		const auto deadline = QuackapiCurrentDeadline(context);
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
				while (!stopping && queue.size() >= WaitingQueueCapacity(EffectiveConcurrency(db))) {
					CheckCaller(context, deadline);
					space_cv.wait_for(guard, std::chrono::milliseconds(10));
				}
				CheckCaller(context, deadline);
				if (stopping || batch->cancelled.load()) {
					throw InterruptException();
				}
				queue.push_back(std::move(job));
				{
					std::lock_guard<std::mutex> batch_guard(batch->mutex);
					batch->accepted++;
				}
				const auto concurrency = EffectiveConcurrency(db);
				EnsureWorkers(std::min(concurrency, active + queue.size()));
				queue_cv.notify_one();
			}
		} catch (InterruptException &) {
			CancelBatch(batch);
			throw;
		} catch (...) {
			auto error = std::current_exception();
			CancelBatch(batch);
			std::rethrow_exception(error);
		}

		const auto expected = Accepted(batch);
		while (true) {
			{
				std::lock_guard<std::mutex> guard(batch->mutex);
				if (batch->completed >= expected) {
					break;
				}
			}
			if (context.IsInterrupted() || IsExpired(deadline) || batch->interrupted.load()) {
				CancelBatch(batch);
				throw InterruptException();
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

	void EnsureWorkers(idx_t desired) {
		while (workers.size() < desired) {
			workers.emplace_back(make_uniq<Worker>());
			auto *worker = workers.back().get();
			try {
				worker->thread = std::thread([this, worker]() { WorkerLoop(*worker); });
			} catch (...) {
				workers.pop_back();
				throw;
			}
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
		queue_cv.notify_all();
		batch->cv.notify_all();
	}

	void CancelBatch(const shared_ptr<PostBatch> &batch) {
		batch->cancelled.store(true);
		for (const auto &flag : batch->cancellation_flags) {
			if (flag) {
				flag->store(true);
			}
		}
		// Queued jobs are removed below. Active jobs retain their shared batch and
		// owned request until provider I/O returns, so the SQL caller need not drain them.
		// An in-flight POST can still complete remotely; DB teardown joins all workers.
		CancelQueued(batch);
	}

	void WorkerLoop(Worker &worker) {
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
				for (;;) {
					if (stopping && queue.empty()) {
						break;
					}
					if (!stopping && !queue.empty()) {
						const auto desired = std::min(EffectiveConcurrency(db), active + queue.size());
						try {
							EnsureWorkers(desired);
						} catch (...) {
							// Existing workers can still drain the queue if growth hits a system limit.
						}
					}
					if (!queue.empty() && (stopping || active < EffectiveConcurrency(db))) {
						break;
					}
					queue_cv.wait_for(guard, std::chrono::milliseconds(10));
				}
				if (stopping && queue.empty()) {
					break;
				}
				job = std::move(queue.front());
				queue.pop_front();
				active++;
				worker.active_job = job;
				space_cv.notify_one();
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
				worker.active_job.reset();
			}
			queue_cv.notify_one();
			batch.cv.notify_all();
			if (batch.cancelled.load()) {
				CancelBatch(job->batch);
			}
		}
		state.reset();
	}

	DatabaseInstance &db;
	std::mutex lock;
	std::condition_variable queue_cv;
	std::condition_variable space_cv;
	std::deque<shared_ptr<PostJob>> queue;
	vector<unique_ptr<Worker>> workers;
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
