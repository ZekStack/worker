#include "Worker.h"

#include <strata/freertos/Mutex.h>
#include <strata/freertos/Queue.h>
#include <strata/freertos/Task.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

namespace {
constexpr WorkerJobId kInvalidJobId = 0;
constexpr uint32_t kWaitPollMs = 10;
constexpr size_t kMaxTaskNameLength = 32;
constexpr size_t kCompletionCapacity = 16;
constexpr size_t kMinStackSizeBytes = 1024;
constexpr const char *kCleanupTaskName = "worker-cleanup";

uint32_t nowMs() {
	return static_cast<uint32_t>(millis());
}

bool elapsedSince(uint32_t startMs, uint32_t timeoutMs) {
	return timeoutMs != UINT32_MAX &&
	       static_cast<uint32_t>(nowMs() - startMs) >= timeoutMs;
}

uint32_t remainingSince(uint32_t startMs, uint32_t durationMs) {
	if (durationMs == UINT32_MAX) {
		return UINT32_MAX;
	}
	const uint32_t elapsedMs = static_cast<uint32_t>(nowMs() - startMs);
	return elapsedMs >= durationMs ? 0 : durationMs - elapsedMs;
}

TickType_t waitTicks(uint32_t durationMs) {
	return durationMs == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(durationMs);
}

bool isValidStackSize(size_t stackBytes) {
	return stackBytes >= kMinStackSizeBytes && (stackBytes % sizeof(StackType_t)) == 0;
}

bool isExecutionCompleteState(WorkerJobState state) {
	switch (state) {
	case WorkerJobState::CallbackComplete:
	case WorkerJobState::CleanupQueued:
	case WorkerJobState::CleanupComplete:
	case WorkerJobState::Stopped:
	case WorkerJobState::Finished:
	case WorkerJobState::Failed:
		return true;
	case WorkerJobState::Created:
	case WorkerJobState::Running:
	case WorkerJobState::Sleeping:
	case WorkerJobState::Stopping:
		return false;
	}
	return false;
}

void copyTaskName(char *destination, size_t destinationSize, const char *source) {
	if (destination == nullptr || destinationSize == 0 || source == nullptr || *source == '\0') {
		return;
	}
	std::strncpy(destination, source, destinationSize - 1);
	destination[destinationSize - 1] = '\0';
}

class WorkerLock {
  public:
	explicit WorkerLock(Strata::FreeRTOS::RecursiveMutex &mutex)
	    : _mutex(mutex), _locked(mutex.lock()) {
	}

	~WorkerLock() {
		if (_locked) {
			_mutex.unlock();
		}
	}

	WorkerLock(const WorkerLock &) = delete;
	WorkerLock &operator=(const WorkerLock &) = delete;

	explicit operator bool() const {
		return _locked;
	}

  private:
	Strata::FreeRTOS::RecursiveMutex &_mutex;
	bool _locked = false;
};

[[noreturn]] void suspendForever() {
	vTaskSuspend(nullptr);
	for (;;) {
		vTaskDelay(portMAX_DELAY);
	}
}
} // namespace

struct WorkerJobRecord {
	WorkerImpl *owner = nullptr;
	WorkerJobId id = kInvalidJobId;
	char name[kMaxTaskNameLength] = "worker-job";
	WorkerCallback callback;
	bool recurring = false;
	uint32_t intervalMs = 0;
	uint32_t stackSize = 0;
	UBaseType_t priority = 0;
	BaseType_t coreId = tskNO_AFFINITY;
	Strata::Placement requestedStackPlacement = Strata::Placement::Default;
	Strata::FreeRTOS::Task task;
	std::atomic<bool> stopRequested{false};
	std::atomic<bool> readyForDelete{false};
	WorkerJobState state = WorkerJobState::Created;
	WorkerJobState finalState = WorkerJobState::Finished;
	bool hasStarted = false;
	uint32_t runCount = 0;
	uint32_t startedAtMs = 0;
	uint32_t lastRunAtMs = 0;
	uint32_t finishedAtMs = 0;
	uint32_t sleepStartMs = 0;
	uint32_t sleepDurationMs = 0;
	bool hasSleepDeadline = false;
	size_t stackHighWaterMarkBytes = 0;
};

struct WorkerCompletion {
	WorkerJobId jobId = kInvalidJobId;
	WorkerJobState finalState = WorkerJobState::Finished;
};

struct WorkerCleanupRequest {
	WorkerJobId jobId = kInvalidJobId;
	WorkerJobRecord *record = nullptr;
	bool stopCleanupTask = false;
};

using WorkerJobPtr = Strata::UniquePtr<WorkerJobRecord>;
using WorkerJobs = Strata::Vector<WorkerJobPtr>;
using WorkerCompletions = Strata::Vector<WorkerCompletion>;
using WorkerCleanupQueue = Strata::FreeRTOS::Queue<WorkerCleanupRequest>;

struct WorkerImpl {
	WorkerImpl() noexcept : mutex(Strata::FreeRTOS::RecursiveMutex::create()) {
	}

	WorkerConfig config{};
	Strata::FreeRTOS::RecursiveMutex mutex;
	std::optional<WorkerJobs> jobs;
	std::optional<WorkerCompletions> completions;
	std::shared_ptr<WorkerEventCallback> onEvent;
	WorkerCleanupQueue cleanupQueue;
	Strata::FreeRTOS::Task cleanupTask;
	bool cleanupTaskRunning = false;
	bool cleanupTaskStopRequested = false;
	std::atomic<bool> cleanupTaskReadyForDelete{false};
	uint32_t cleanupQueueHighWaterMark = 0;
	bool initialized = false;
	bool ending = false;
	WorkerJobId nextJobId = 1;

	void initializeStorage(Strata::Placement placement, size_t maxConcurrentJobs) {
		jobs.reset();
		completions.reset();
		jobs.emplace(Strata::Allocator<WorkerJobPtr>{placement});
		completions.emplace(Strata::Allocator<WorkerCompletion>{placement});
		jobs->reserve(maxConcurrentJobs);
		completions->reserve(kCompletionCapacity);
	}

	WorkerResult emitResult(WorkerResult result, WorkerJobId jobId = kInvalidJobId) {
		if (!result) {
			emitEvent(WorkerEventType::Error, result.status, jobId, result.message);
		}
		return result;
	}

	WorkerJobResult emitJobResult(WorkerJobResult result) {
		if (!result) {
			emitEvent(WorkerEventType::Error, result.status, result.jobId, result.message);
		}
		return result;
	}

	void emitEvent(
	    WorkerEventType type,
	    WorkerStatus status,
	    WorkerJobId jobId,
	    const char *message
	) {
		std::shared_ptr<WorkerEventCallback> callback;
		{
			WorkerLock lock(mutex);
			if (!lock) {
				return;
			}
			callback = onEvent;
		}
		if (callback && *callback) {
			(*callback)(WorkerEvent{type, status, jobId, message != nullptr ? message : "event"});
		}
	}

	WorkerJobRecord *findJob(WorkerJobId jobId) {
		if (!jobs) {
			return nullptr;
		}
		for (auto &job : *jobs) {
			if (job && job->id == jobId) {
				return job.get();
			}
		}
		return nullptr;
	}

	bool hasCompletion(WorkerJobId jobId) const {
		if (!completions) {
			return false;
		}
		return std::any_of(
		    completions->begin(),
		    completions->end(),
		    [jobId](const WorkerCompletion &completion) { return completion.jobId == jobId; }
		);
	}

	bool consumeCompletion(WorkerJobId jobId, WorkerJobState &finalState) {
		if (!completions) {
			return false;
		}
		auto it = std::find_if(
		    completions->begin(),
		    completions->end(),
		    [jobId](const WorkerCompletion &completion) { return completion.jobId == jobId; }
		);
		if (it == completions->end()) {
			return false;
		}
		finalState = it->finalState;
		completions->erase(it);
		return true;
	}

	void recordCompletion(WorkerJobId jobId, WorkerJobState finalState) {
		if (!completions) {
			return;
		}
		completions->erase(
		    std::remove_if(
		        completions->begin(),
		        completions->end(),
		        [jobId](const WorkerCompletion &completion) { return completion.jobId == jobId; }
		    ),
		    completions->end()
		);
		if (completions->size() >= kCompletionCapacity) {
			completions->erase(completions->begin());
		}
		completions->push_back(WorkerCompletion{jobId, finalState});
	}

	void eraseJob(WorkerJobId jobId) {
		if (!jobs) {
			return;
		}
		jobs->erase(
		    std::remove_if(
		        jobs->begin(),
		        jobs->end(),
		        [jobId](const WorkerJobPtr &job) { return job && job->id == jobId; }
		    ),
		    jobs->end()
		);
	}

	WorkerJobId allocateJobId() {
		WorkerJobId id = nextJobId++;
		if (id == kInvalidJobId) {
			id = nextJobId++;
		}
		return id;
	}

	WorkerResult completionResult(WorkerJobState finalState) {
		if (finalState == WorkerJobState::Failed) {
			return WorkerResult::failure(WorkerStatus::InternalError, "job failed");
		}
		return WorkerResult::success(
		    finalState == WorkerJobState::Stopped ? "job stopped" : "job finished"
		);
	}

	WorkerResult waitForJobId(WorkerJobId jobId, uint32_t timeoutMs) {
		const uint32_t startMs = nowMs();
		while (true) {
			WorkerJobState finalState = WorkerJobState::Failed;
			bool foundActive = false;
			{
				WorkerLock lock(mutex);
				if (!lock) {
					return emitResult(
					    WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker"),
					    jobId
					);
				}
				if (consumeCompletion(jobId, finalState)) {
					return completionResult(finalState);
				}
				foundActive = findJob(jobId) != nullptr;
			}

			if (!foundActive) {
				return emitResult(
				    WorkerResult::failure(WorkerStatus::JobNotFound, "job not found"),
				    jobId
				);
			}
			if (elapsedSince(startMs, timeoutMs)) {
				return emitResult(
				    WorkerResult::failure(WorkerStatus::Timeout, "wait timed out"),
				    jobId
				);
			}
			vTaskDelay(pdMS_TO_TICKS(kWaitPollMs));
		}
	}

	WorkerJobConfig defaultJobConfig() const {
		WorkerJobConfig jobConfig;
		jobConfig.stackSize = config.defaultStackSize;
		jobConfig.priority = config.defaultPriority;
		jobConfig.coreId = config.defaultCoreId;
		return jobConfig;
	}

	WorkerJobConfig resolveJobConfig(const WorkerJobConfig &jobConfig) const {
		WorkerJobConfig resolved = jobConfig;
		if (resolved.stackSize == 0) {
			resolved.stackSize = config.defaultStackSize;
		}
		if (resolved.priority == 0) {
			resolved.priority = config.defaultPriority;
		}
		return resolved;
	}

	Strata::Placement resolveJobStackPlacement(const WorkerJobConfig &jobConfig) const {
		return jobConfig.stackPlacement.value_or(config.memory.taskStack);
	}

	Strata::Placement resolveCleanupStackPlacement(const WorkerConfig &incomingConfig) const {
		return incomingConfig.cleanupTaskStackPlacement.value_or(incomingConfig.memory.taskStack);
	}

	void markRunStart(WorkerJobRecord *job) {
		WorkerLock lock(mutex);
		if (!lock || job == nullptr) {
			return;
		}
		const uint32_t currentMs = nowMs();
		if (!job->hasStarted) {
			job->hasStarted = true;
			job->startedAtMs = currentMs;
		}
		job->lastRunAtMs = currentMs;
		job->runCount++;
		job->state = WorkerJobState::Running;
	}

	bool waitWhileSleeping(WorkerJobRecord *job, uint32_t durationMs) {
		if (job == nullptr || durationMs == 0) {
			return true;
		}
		return waitForDuration(job, durationMs);
	}

	bool waitForDuration(WorkerJobRecord *job, uint32_t durationMs) {
		if (job == nullptr) {
			return false;
		}
		const uint32_t startMs = nowMs();
		while (!job->stopRequested.load()) {
			uint32_t remainingMs = remainingSince(startMs, durationMs);
			{
				WorkerLock lock(mutex);
				if (lock) {
					uint32_t externalRemainingMs = 0;
					if (job->hasSleepDeadline) {
						externalRemainingMs =
						    remainingSince(job->sleepStartMs, job->sleepDurationMs);
						if (externalRemainingMs == 0) {
							job->hasSleepDeadline = false;
						}
					}
					remainingMs = std::max(remainingMs, externalRemainingMs);
					if (!isExecutionCompleteState(job->state)) {
						job->state = WorkerJobState::Sleeping;
					}
				}
			}

			if (remainingMs == 0) {
				break;
			}
			ulTaskNotifyTake(pdTRUE, waitTicks(remainingMs));
		}
		return !job->stopRequested.load();
	}

	WorkerJobState executeJob(WorkerJobRecord *job) {
		WorkerCallback callback;
		{
			WorkerLock lock(mutex);
			if (!lock || job == nullptr) {
				return WorkerJobState::Failed;
			}
			callback = std::move(job->callback);
			job->callback = {};
		}
		if (!callback) {
			return WorkerJobState::Failed;
		}

		WorkerJobContext context(job);
		WorkerJobState finalState = WorkerJobState::Finished;
		if (job->recurring) {
			while (!job->stopRequested.load()) {
				markRunStart(job);
				callback(context);
				if (job->stopRequested.load()) {
					break;
				}
				waitForDuration(job, job->intervalMs);
			}
			finalState = WorkerJobState::Stopped;
		} else {
			markRunStart(job);
			callback(context);
			finalState =
			    job->stopRequested.load() ? WorkerJobState::Stopped : WorkerJobState::Finished;
		}
		return finalState;
	}

	void prepareCleanup(WorkerJobRecord *job, WorkerJobState finalState) {
		WorkerLock lock(mutex);
		if (!lock || job == nullptr) {
			return;
		}
		job->stackHighWaterMarkBytes = job->task.stackHighWaterMarkBytes();
		job->finalState = finalState;
		job->state = WorkerJobState::CallbackComplete;
		job->finishedAtMs = nowMs();
	}

	void queueCleanup(WorkerJobRecord *job) {
		if (job == nullptr || !cleanupQueue) {
			return;
		}
		{
			WorkerLock lock(mutex);
			if (lock) {
				job->state = WorkerJobState::CleanupQueued;
			}
		}

		const WorkerCleanupRequest request{job->id, job, false};
		while (!cleanupQueue.send(request, portMAX_DELAY)) {
			vTaskDelay(1);
		}

		const uint32_t queueDepth =
		    static_cast<uint32_t>(uxQueueMessagesWaiting(cleanupQueue.handle()));
		WorkerLock lock(mutex);
		if (lock) {
			cleanupQueueHighWaterMark = std::max(cleanupQueueHighWaterMark, queueDepth);
		}
	}

	void completeCleanup(const WorkerCleanupRequest &request) {
		WorkerJobState finalState = WorkerJobState::Failed;
		bool found = false;
		{
			WorkerLock lock(mutex);
			if (!lock) {
				return;
			}
			WorkerJobRecord *job = findJob(request.jobId);
			if (job == request.record && job != nullptr) {
				job->state = WorkerJobState::CleanupComplete;
				finalState = job->finalState;
				recordCompletion(job->id, finalState);
				eraseJob(job->id);
				found = true;
			}
		}
		if (!found) {
			return;
		}
		if (finalState == WorkerJobState::Stopped) {
			emitEvent(WorkerEventType::Info, WorkerStatus::Ok, request.jobId, "job stopped");
		} else if (finalState == WorkerJobState::Finished) {
			emitEvent(WorkerEventType::Info, WorkerStatus::Ok, request.jobId, "job finished");
		} else {
			emitEvent(
			    WorkerEventType::Error,
			    WorkerStatus::InternalError,
			    request.jobId,
			    "job failed"
			);
		}
	}

	WorkerJobResult startJob(
	    const WorkerJobConfig &incomingConfig,
	    WorkerCallback callback,
	    bool recurring,
	    uint32_t intervalMs
	) {
		if (!callback) {
			return emitJobResult(WorkerJobResult::failure(
			    WorkerStatus::InvalidArgument,
			    "callback is required"
			));
		}
		if (recurring && intervalMs == 0) {
			return emitJobResult(WorkerJobResult::failure(
			    WorkerStatus::InvalidArgument,
			    "interval must be greater than zero"
			));
		}
		if (incomingConfig.stackPlacement &&
		    !Strata::validPlacement(*incomingConfig.stackPlacement)) {
			return emitJobResult(WorkerJobResult::failure(
			    WorkerStatus::InvalidArgument,
			    "invalid stack placement"
			));
		}

		const WorkerJobConfig jobConfig = resolveJobConfig(incomingConfig);
		if (!isValidStackSize(jobConfig.stackSize)) {
			return emitJobResult(WorkerJobResult::failure(
			    WorkerStatus::InvalidArgument,
			    "stack size must be at least 1024 bytes and aligned"
			));
		}
		const Strata::Placement stackPlacement = resolveJobStackPlacement(jobConfig);

		auto job = Strata::makeUnique<WorkerJobRecord>(config.memory.allocation);
		if (!job) {
			return emitJobResult(WorkerJobResult::failure(
			    WorkerStatus::OutOfMemory,
			    "failed to allocate job record"
			));
		}

		job->owner = this;
		job->callback = std::move(callback);
		job->recurring = recurring;
		job->intervalMs = intervalMs;
		job->stackSize = jobConfig.stackSize;
		job->priority = jobConfig.priority;
		job->coreId = jobConfig.coreId;
		job->requestedStackPlacement = stackPlacement;
		if (jobConfig.name != nullptr && *jobConfig.name != '\0') {
			copyTaskName(job->name, sizeof(job->name), jobConfig.name);
		}

		WorkerJobResult result;
		{
			WorkerLock lock(mutex);
			if (!lock) {
				return emitJobResult(WorkerJobResult::failure(
				    WorkerStatus::InternalError,
				    "failed to lock worker registry"
				));
			}
			if (!initialized || !cleanupQueue || !cleanupTask) {
				return emitJobResult(WorkerJobResult::failure(
				    WorkerStatus::NotInitialized,
				    "worker is not initialized"
				));
			}
			if (ending) {
				return emitJobResult(WorkerJobResult::failure(
				    WorkerStatus::Busy,
				    "worker is ending"
				));
			}
			if (!jobs || jobs->size() >= config.maxConcurrentJobs) {
				return emitJobResult(WorkerJobResult::failure(
				    WorkerStatus::Busy,
				    "maximum concurrent jobs reached"
				));
			}

			job->id = allocateJobId();
			WorkerJobRecord *jobRecord = job.get();
			const WorkerJobId jobId = job->id;
			jobs->push_back(std::move(job));

			auto task = Strata::FreeRTOS::Task::create(
			    &WorkerImpl::taskEntry,
			    jobRecord,
			    Strata::FreeRTOS::TaskConfig{
			        .name = jobRecord->name,
			        .stackBytes = jobRecord->stackSize,
			        .stackPlacement = stackPlacement,
			        .priority = jobRecord->priority,
			        .affinity = jobRecord->coreId,
			    }
			);
			if (!task) {
				eraseJob(jobId);
				result = WorkerJobResult::failure(
				    WorkerStatus::TaskCreateFailed,
				    "failed to create job task",
				    jobId
				);
			} else {
				jobRecord->task = std::move(task);
				xTaskNotifyGive(jobRecord->task.handle());
				result = WorkerJobResult::success(jobId, "job started");
			}
		}
		return emitJobResult(result);
	}

	bool initializeCleanupInfrastructure(const WorkerConfig &incomingConfig) {
		cleanupQueue = WorkerCleanupQueue::create({
		    .length = incomingConfig.maxConcurrentJobs,
		    .storagePlacement = incomingConfig.memory.allocation,
		    .usage = Strata::FreeRTOS::QueueUsage::TaskOnly,
		});
		if (!cleanupQueue) {
			return false;
		}

		cleanupTaskReadyForDelete.store(false);
		cleanupTaskStopRequested = false;
		cleanupTaskRunning = false;
		cleanupQueueHighWaterMark = 0;
		const Strata::Placement stackPlacement = resolveCleanupStackPlacement(incomingConfig);
		auto task = Strata::FreeRTOS::Task::create(
		    &WorkerImpl::cleanupTaskEntry,
		    this,
		    Strata::FreeRTOS::TaskConfig{
		        .name = kCleanupTaskName,
		        .stackBytes = incomingConfig.cleanupTaskStackSize,
		        .stackPlacement = stackPlacement,
		        .priority = incomingConfig.cleanupTaskPriority,
		        .affinity = incomingConfig.cleanupTaskCoreId,
		    }
		);
		if (!task) {
			cleanupQueue.reset();
			return false;
		}
		cleanupTask = std::move(task);
		xTaskNotifyGive(cleanupTask.handle());
		return true;
	}

	WorkerResult stopCleanupInfrastructure(uint32_t startMs, uint32_t timeoutMs) {
		bool sendStop = false;
		{
			WorkerLock lock(mutex);
			if (!lock) {
				return WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker");
			}
			if (!cleanupQueue || !cleanupTask) {
				return WorkerResult::success("cleanup task already stopped");
			}
			if (!cleanupTaskStopRequested) {
				cleanupTaskStopRequested = true;
				sendStop = true;
			}
		}

		if (sendStop) {
			const WorkerCleanupRequest stopRequest{kInvalidJobId, nullptr, true};
			if (!cleanupQueue.send(stopRequest, 0)) {
				WorkerLock lock(mutex);
				if (lock) {
					cleanupTaskStopRequested = false;
				}
				return WorkerResult::failure(
				    WorkerStatus::InternalError,
				    "failed to stop cleanup task"
				);
			}
		}

		while (!cleanupTaskReadyForDelete.load(std::memory_order_acquire)) {
			if (elapsedSince(startMs, timeoutMs)) {
				return WorkerResult::failure(WorkerStatus::Timeout, "worker end timed out");
			}
			vTaskDelay(pdMS_TO_TICKS(kWaitPollMs));
		}

		vTaskSuspend(cleanupTask.handle());
		cleanupTask.reset();
		cleanupQueue.reset();
		{
			WorkerLock lock(mutex);
			if (lock) {
				cleanupTaskRunning = false;
				cleanupTaskStopRequested = false;
				cleanupTaskReadyForDelete.store(false);
			}
		}
		return WorkerResult::success("cleanup task stopped");
	}

	static void taskEntry(void *arg) {
		auto *job = static_cast<WorkerJobRecord *>(arg);
		if (job == nullptr || job->owner == nullptr) {
			suspendForever();
		}

		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		WorkerImpl *owner = job->owner;
		const WorkerJobState finalState = owner->executeJob(job);
		owner->prepareCleanup(job, finalState);
		owner->queueCleanup(job);

		job->readyForDelete.store(true, std::memory_order_release);
		suspendForever();
	}

	static void cleanupTaskEntry(void *arg) {
		auto *owner = static_cast<WorkerImpl *>(arg);
		if (owner == nullptr) {
			suspendForever();
		}

		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		{
			WorkerLock lock(owner->mutex);
			if (lock) {
				owner->cleanupTaskRunning = true;
			}
		}

		for (;;) {
			WorkerCleanupRequest request;
			if (!owner->cleanupQueue.receive(request, portMAX_DELAY)) {
				continue;
			}
			if (request.stopCleanupTask) {
				{
					WorkerLock lock(owner->mutex);
					if (lock) {
						owner->cleanupTaskRunning = false;
					}
				}
				owner->cleanupTaskReadyForDelete.store(true, std::memory_order_release);
				suspendForever();
			}

			if (request.record == nullptr || !request.record->task) {
				continue;
			}
			while (!request.record->readyForDelete.load(std::memory_order_acquire)) {
				taskYIELD();
			}
			vTaskSuspend(request.record->task.handle());
			request.record->task.reset();
			owner->completeCleanup(request);
		}
	}
};

WorkerResult WorkerResult::success(const char *message) {
	WorkerResult result;
	result.result = true;
	result.status = WorkerStatus::Ok;
	result.message = message != nullptr ? message : "ok";
	return result;
}

WorkerResult WorkerResult::failure(WorkerStatus status, const char *message) {
	WorkerResult result;
	result.result = false;
	result.status = status;
	result.message = message != nullptr ? message : "error";
	return result;
}

WorkerJobResult WorkerJobResult::success(WorkerJobId jobId, const char *message) {
	WorkerJobResult result;
	result.result = true;
	result.status = WorkerStatus::Ok;
	result.message = message != nullptr ? message : "ok";
	result.jobId = jobId;
	return result;
}

WorkerJobResult WorkerJobResult::failure(
    WorkerStatus status,
    const char *message,
    WorkerJobId jobId
) {
	WorkerJobResult result;
	result.result = false;
	result.status = status;
	result.message = message != nullptr ? message : "error";
	result.jobId = jobId;
	return result;
}

WorkerJobContext::WorkerJobContext(WorkerJobRecord *record) : _record(record) {
}

WorkerJobId WorkerJobContext::id() const {
	return _record != nullptr ? _record->id : kInvalidJobId;
}

void WorkerJobContext::stop() {
	if (_record != nullptr) {
		_record->stopRequested.store(true);
	}
}

void WorkerJobContext::sleep(uint32_t durationMs) {
	if (_record == nullptr || _record->owner == nullptr) {
		return;
	}
	_record->owner->waitWhileSleeping(_record, durationMs);
}

bool WorkerJobContext::shouldStop() const {
	return _record == nullptr || _record->stopRequested.load();
}

uint32_t WorkerJobContext::runCount() const {
	if (_record == nullptr || _record->owner == nullptr) {
		return 0;
	}
	WorkerLock lock(_record->owner->mutex);
	return lock ? _record->runCount : 0;
}

uint64_t WorkerJobContext::startedAtMs() const {
	if (_record == nullptr || _record->owner == nullptr) {
		return 0;
	}
	WorkerLock lock(_record->owner->mutex);
	return lock ? _record->startedAtMs : 0;
}

uint64_t WorkerJobContext::lastRunAtMs() const {
	if (_record == nullptr || _record->owner == nullptr) {
		return 0;
	}
	WorkerLock lock(_record->owner->mutex);
	return lock ? _record->lastRunAtMs : 0;
}

Worker::Worker() : _impl(Strata::makeUnique<WorkerImpl>(Strata::Placement::Internal)) {
}

Worker::~Worker() {
	if (_impl) {
		end(UINT32_MAX);
	}
}

WorkerResult Worker::init(const WorkerConfig &config) {
	if (!_impl) {
		return WorkerResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	if (!_impl->mutex) {
		return WorkerResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker mutex");
	}
	if (!Strata::validMemoryPolicy(config.memory) ||
	    (config.cleanupTaskStackPlacement &&
	     !Strata::validPlacement(*config.cleanupTaskStackPlacement))) {
		return _impl->emitResult(WorkerResult::failure(
		    WorkerStatus::InvalidArgument,
		    "invalid memory placement"
		));
	}
	if (!isValidStackSize(config.defaultStackSize) ||
	    !isValidStackSize(config.cleanupTaskStackSize)) {
		return _impl->emitResult(WorkerResult::failure(
		    WorkerStatus::InvalidArgument,
		    "task stack sizes must be at least 1024 bytes and aligned"
		));
	}
	if (config.maxConcurrentJobs == 0) {
		return _impl->emitResult(WorkerResult::failure(
		    WorkerStatus::InvalidArgument,
		    "maximum concurrent jobs must be greater than zero"
		));
	}

	WorkerResult failure;
	bool hasFailure = false;
	{
		WorkerLock lock(_impl->mutex);
		if (!lock) {
			return WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker");
		}
		if (_impl->initialized) {
			failure = WorkerResult::failure(
			    WorkerStatus::AlreadyInitialized,
			    "worker already initialized"
			);
			hasFailure = true;
		} else {
			_impl->config = config;
			_impl->ending = false;
			_impl->nextJobId = 1;
			_impl->initializeStorage(config.memory.allocation, config.maxConcurrentJobs);
			if (!_impl->initializeCleanupInfrastructure(config)) {
				failure = WorkerResult::failure(
				    WorkerStatus::TaskCreateFailed,
				    "failed to initialize cleanup task"
				);
				hasFailure = true;
			} else {
				_impl->initialized = true;
			}
		}
	}
	if (hasFailure) {
		return _impl->emitResult(failure);
	}
	_impl->emitEvent(WorkerEventType::Info, WorkerStatus::Ok, kInvalidJobId, "worker initialized");
	return WorkerResult::success("worker initialized");
}

void Worker::onEvent(WorkerEventCallback callback) {
	if (!_impl) {
		return;
	}
	std::shared_ptr<WorkerEventCallback> holder;
	if (callback) {
		holder = Strata::makeShared<WorkerEventCallback>(
		    Strata::Placement::Internal,
		    std::move(callback)
		);
	}
	WorkerLock lock(_impl->mutex);
	if (lock) {
		_impl->onEvent = std::move(holder);
	}
}

WorkerJobResult Worker::once(WorkerCallback callback) {
	if (!_impl) {
		return WorkerJobResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	return _impl->startJob(_impl->defaultJobConfig(), std::move(callback), false, 0);
}

WorkerJobResult Worker::once(const WorkerJobConfig &config, WorkerCallback callback) {
	if (!_impl) {
		return WorkerJobResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	return _impl->startJob(config, std::move(callback), false, 0);
}

WorkerJobResult Worker::every(uint32_t intervalMs, WorkerCallback callback) {
	if (!_impl) {
		return WorkerJobResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	return _impl->startJob(
	    _impl->defaultJobConfig(),
	    std::move(callback),
	    true,
	    intervalMs
	);
}

WorkerJobResult Worker::every(
    uint32_t intervalMs,
    const WorkerJobConfig &config,
    WorkerCallback callback
) {
	if (!_impl) {
		return WorkerJobResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	return _impl->startJob(config, std::move(callback), true, intervalMs);
}

WorkerResult Worker::stop(WorkerJobId jobId) {
	if (!_impl) {
		return WorkerResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	TaskHandle_t handle = nullptr;
	WorkerResult failure;
	bool hasFailure = false;
	bool alreadyFinished = false;
	{
		WorkerLock lock(_impl->mutex);
		if (!lock) {
			failure = WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker");
			hasFailure = true;
		} else {
			WorkerJobRecord *job = _impl->findJob(jobId);
			if (job == nullptr) {
				if (_impl->hasCompletion(jobId)) {
					alreadyFinished = true;
				} else {
					failure = WorkerResult::failure(WorkerStatus::JobNotFound, "job not found");
					hasFailure = true;
				}
			} else if (isExecutionCompleteState(job->state)) {
				alreadyFinished = true;
			} else {
				job->stopRequested.store(true);
				job->state = WorkerJobState::Stopping;
				handle = job->task.handle();
			}
		}
	}
	if (hasFailure) {
		return _impl->emitResult(failure, jobId);
	}
	if (alreadyFinished) {
		return WorkerResult::success("job already finished");
	}
	if (handle != nullptr) {
		xTaskNotifyGive(handle);
	}
	return WorkerResult::success("job stop requested");
}

WorkerResult Worker::stopAndWait(WorkerJobId jobId, uint32_t timeoutMs) {
	if (!_impl) {
		return WorkerResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	TaskHandle_t handle = nullptr;
	WorkerResult failure;
	bool hasFailure = false;
	{
		WorkerLock lock(_impl->mutex);
		if (!lock) {
			failure = WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker");
			hasFailure = true;
		} else {
			WorkerJobRecord *job = _impl->findJob(jobId);
			if (job == nullptr) {
				if (!_impl->hasCompletion(jobId)) {
					failure = WorkerResult::failure(WorkerStatus::JobNotFound, "job not found");
					hasFailure = true;
				}
			} else if (!isExecutionCompleteState(job->state)) {
				job->stopRequested.store(true);
				job->state = WorkerJobState::Stopping;
				handle = job->task.handle();
			}
		}
	}
	if (hasFailure) {
		return _impl->emitResult(failure, jobId);
	}
	if (handle != nullptr) {
		xTaskNotifyGive(handle);
	}
	return _impl->waitForJobId(jobId, timeoutMs);
}

WorkerResult Worker::sleep(WorkerJobId jobId, uint32_t durationMs) {
	if (!_impl) {
		return WorkerResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	if (durationMs == 0) {
		return _impl->emitResult(
		    WorkerResult::failure(WorkerStatus::InvalidArgument, "sleep duration is required"),
		    jobId
		);
	}

	TaskHandle_t handle = nullptr;
	WorkerResult failure;
	bool hasFailure = false;
	{
		WorkerLock lock(_impl->mutex);
		if (!lock) {
			failure = WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker");
			hasFailure = true;
		} else {
			WorkerJobRecord *job = _impl->findJob(jobId);
			if (job == nullptr) {
				failure = _impl->hasCompletion(jobId)
				    ? WorkerResult::failure(WorkerStatus::InvalidArgument, "job already finished")
				    : WorkerResult::failure(WorkerStatus::JobNotFound, "job not found");
				hasFailure = true;
			} else if (isExecutionCompleteState(job->state)) {
				failure = WorkerResult::failure(WorkerStatus::InvalidArgument, "job already finished");
				hasFailure = true;
			} else {
				const uint32_t existingRemainingMs = job->hasSleepDeadline
				    ? remainingSince(job->sleepStartMs, job->sleepDurationMs)
				    : 0;
				job->sleepStartMs = nowMs();
				job->sleepDurationMs = std::max(existingRemainingMs, durationMs);
				job->hasSleepDeadline = true;
				job->state = WorkerJobState::Sleeping;
				handle = job->task.handle();
			}
		}
	}
	if (hasFailure) {
		return _impl->emitResult(failure, jobId);
	}
	if (handle != nullptr) {
		xTaskNotifyGive(handle);
	}
	return WorkerResult::success("job sleep requested");
}

WorkerResult Worker::waitFor(WorkerJobId jobId) {
	return waitFor(jobId, UINT32_MAX);
}

WorkerResult Worker::waitFor(WorkerJobId jobId, uint32_t timeoutMs) {
	if (!_impl) {
		return WorkerResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	return _impl->waitForJobId(jobId, timeoutMs);
}

WorkerResult Worker::clearFinished() {
	if (!_impl) {
		return WorkerResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	return WorkerResult::success("completed jobs are cleaned automatically");
}

WorkerDiag Worker::getDiagnostics() {
	WorkerDiag diag;
	if (!_impl) {
		return diag;
	}
	WorkerLock lock(_impl->mutex);
	if (!lock) {
		return diag;
	}

	diag.activeJobCount = _impl->jobs ? static_cast<uint32_t>(_impl->jobs->size()) : 0;
	diag.cleanupTaskRunning = _impl->cleanupTaskRunning;
	diag.cleanupQueueHighWaterMark = _impl->cleanupQueueHighWaterMark;
	if (_impl->cleanupQueue) {
		diag.cleanupQueueDepth =
		    static_cast<uint32_t>(uxQueueMessagesWaiting(_impl->cleanupQueue.handle()));
		diag.cleanupQueueStoragePlacement = _impl->cleanupQueue.storagePlacement();
		diag.cleanupQueueStorageRegion = _impl->cleanupQueue.storageRegion();
	}
	if (_impl->cleanupTask) {
		diag.cleanupTaskStackPlacement = _impl->cleanupTask.stackPlacement();
		diag.cleanupTaskStackRegion = _impl->cleanupTask.stackRegion();
	}
	if (_impl->jobs) {
		for (const auto &job : *_impl->jobs) {
			if (!job) {
				continue;
			}
			switch (job->state) {
			case WorkerJobState::Running:
				diag.runningJobCount++;
				break;
			case WorkerJobState::Sleeping:
				diag.sleepingJobCount++;
				break;
			case WorkerJobState::Stopping:
				diag.stoppingJobCount++;
				break;
			case WorkerJobState::CallbackComplete:
			case WorkerJobState::CleanupQueued:
				diag.cleanupQueuedCount++;
				break;
			case WorkerJobState::Created:
			case WorkerJobState::CleanupComplete:
			case WorkerJobState::Stopped:
			case WorkerJobState::Finished:
			case WorkerJobState::Failed:
				break;
			}
		}
	}
	return diag;
}

WorkerResult Worker::getJobDiagnostics(WorkerJobId jobId, WorkerJobDiag &out) {
	if (!_impl) {
		return WorkerResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	WorkerResult failure;
	bool hasFailure = false;
	{
		WorkerLock lock(_impl->mutex);
		if (!lock) {
			failure = WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker");
			hasFailure = true;
		} else {
			WorkerJobRecord *job = _impl->findJob(jobId);
			if (job == nullptr) {
				failure = WorkerResult::failure(WorkerStatus::JobNotFound, "job not found");
				hasFailure = true;
			} else {
				out.jobId = job->id;
				out.state = job->state;
				out.name = job->name;
				out.stackSize = job->stackSize;
				out.priority = job->priority;
				out.coreId = job->coreId;
				out.requestedStackPlacement = job->requestedStackPlacement;
				out.stackRegion = job->task ? job->task.stackRegion() : Strata::Region::Unknown;
				out.runCount = job->runCount;
				out.startedAtMs = job->startedAtMs;
				out.lastRunAtMs = job->lastRunAtMs;
				out.finishedAtMs = job->finishedAtMs;
				out.stackHighWaterMarkBytes = job->task
				    ? job->task.stackHighWaterMarkBytes()
				    : job->stackHighWaterMarkBytes;
			}
		}
	}
	if (hasFailure) {
		return _impl->emitResult(failure, jobId);
	}
	return WorkerResult::success("job diagnostics loaded");
}

WorkerResult Worker::end(uint32_t timeoutMs) {
	if (!_impl) {
		return WorkerResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}

	const uint32_t startMs = nowMs();
	{
		WorkerLock lock(_impl->mutex);
		if (!lock) {
			return WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker");
		}
		if (!_impl->initialized) {
			return WorkerResult::success("worker not initialized");
		}
		_impl->ending = true;
		if (_impl->jobs) {
			for (auto &job : *_impl->jobs) {
				if (!job || isExecutionCompleteState(job->state)) {
					continue;
				}
				job->stopRequested.store(true);
				job->state = WorkerJobState::Stopping;
				if (job->task) {
					xTaskNotifyGive(job->task.handle());
				}
			}
		}
	}

	while (true) {
		bool jobsEmpty = false;
		{
			WorkerLock lock(_impl->mutex);
			if (!lock) {
				return WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker");
			}
			jobsEmpty = !_impl->jobs || _impl->jobs->empty();
		}
		if (jobsEmpty) {
			break;
		}
		if (elapsedSince(startMs, timeoutMs)) {
			return _impl->emitResult(
			    WorkerResult::failure(WorkerStatus::Timeout, "worker end timed out")
			);
		}
		vTaskDelay(pdMS_TO_TICKS(kWaitPollMs));
	}

	WorkerResult cleanupResult = _impl->stopCleanupInfrastructure(startMs, timeoutMs);
	if (!cleanupResult) {
		return _impl->emitResult(cleanupResult);
	}

	{
		WorkerLock lock(_impl->mutex);
		if (lock) {
			if (_impl->jobs) {
				_impl->jobs->clear();
			}
			if (_impl->completions) {
				_impl->completions->clear();
			}
			_impl->nextJobId = 1;
			_impl->initialized = false;
			_impl->ending = false;
			_impl->cleanupQueueHighWaterMark = 0;
		}
	}
	_impl->emitEvent(WorkerEventType::Info, WorkerStatus::Ok, kInvalidJobId, "worker ended");
	return WorkerResult::success("worker ended");
}

const char *Worker::statusToString(WorkerStatus status) const {
	switch (status) {
	case WorkerStatus::Ok:
		return "ok";
	case WorkerStatus::NotInitialized:
		return "notInitialized";
	case WorkerStatus::AlreadyInitialized:
		return "alreadyInitialized";
	case WorkerStatus::InvalidArgument:
		return "invalidArgument";
	case WorkerStatus::OutOfMemory:
		return "outOfMemory";
	case WorkerStatus::TaskCreateFailed:
		return "taskCreateFailed";
	case WorkerStatus::JobNotFound:
		return "jobNotFound";
	case WorkerStatus::Busy:
		return "busy";
	case WorkerStatus::Timeout:
		return "timeout";
	case WorkerStatus::InternalError:
		return "internalError";
	}
	return "unknown";
}

const char *Worker::eventTypeToString(WorkerEventType type) const {
	switch (type) {
	case WorkerEventType::Info:
		return "info";
	case WorkerEventType::Warning:
		return "warning";
	case WorkerEventType::Error:
		return "error";
	}
	return "unknown";
}

const char *Worker::jobStateToString(WorkerJobState state) const {
	switch (state) {
	case WorkerJobState::Created:
		return "created";
	case WorkerJobState::Running:
		return "running";
	case WorkerJobState::Sleeping:
		return "sleeping";
	case WorkerJobState::Stopping:
		return "stopping";
	case WorkerJobState::Stopped:
		return "stopped";
	case WorkerJobState::Finished:
		return "finished";
	case WorkerJobState::Failed:
		return "failed";
	case WorkerJobState::CallbackComplete:
		return "callbackComplete";
	case WorkerJobState::CleanupQueued:
		return "cleanupQueued";
	case WorkerJobState::CleanupComplete:
		return "cleanupComplete";
	}
	return "unknown";
}
