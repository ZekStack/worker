#include "Worker.h"

#include "internal/WorkerMutex.h"
#include "internal/WorkerTaskSupport.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace {
constexpr WorkerJobId kInvalidJobId = 0;
constexpr uint32_t kWaitPollMs = 10;
constexpr size_t kMaxTaskNameLength = 32;

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

bool isTerminalState(WorkerJobState state) {
	return state == WorkerJobState::Stopped || state == WorkerJobState::Finished ||
	       state == WorkerJobState::Failed;
}

bool isReapableJob(const std::shared_ptr<WorkerJobRecord> &job);

void copyTaskName(char *destination, size_t destinationSize, const char *source) {
	if (destination == nullptr || destinationSize == 0 || source == nullptr || *source == '\0') {
		return;
	}
	std::strncpy(destination, source, destinationSize - 1);
	destination[destinationSize - 1] = '\0';
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
	WorkerStackType requestedStackType = WorkerStackType::Auto;
	WorkerStackType actualStackType = WorkerStackType::Internal;
	TaskHandle_t taskHandle = nullptr;
	bool createdWithCaps = false;
	std::atomic<bool> stopRequested{false};
	WorkerJobState state = WorkerJobState::Created;
	bool hasStarted = false;
	uint32_t runCount = 0;
	uint32_t startedAtMs = 0;
	uint32_t lastRunAtMs = 0;
	uint32_t finishedAtMs = 0;
	uint32_t sleepStartMs = 0;
	uint32_t sleepDurationMs = 0;
	bool hasSleepDeadline = false;
	size_t stackHighWaterMarkBytes = 0;
	bool terminalAccounted = false;
	bool taskExited = false;
};

struct TaskEntryExit {
	bool createdWithCaps = false;
	WorkerImpl *owner = nullptr;
	WorkerJobId jobId = kInvalidJobId;
};

namespace {
bool isReapableJob(const std::shared_ptr<WorkerJobRecord> &job) {
	return job && isTerminalState(job->state) && job->taskExited;
}
} // namespace

struct WorkerImpl {
	WorkerConfig config{};
	WorkerMutex mutex;
	std::vector<std::shared_ptr<WorkerJobRecord>> jobs;
	WorkerEventCallback onEvent;
	bool initialized = false;
	bool ending = false;
	WorkerJobId nextJobId = 1;
	uint32_t totalJobCount = 0;
	uint32_t finishedJobCount = 0;
	uint32_t stoppedJobCount = 0;
	uint32_t failedJobCount = 0;
	uint32_t psramStackJobCount = 0;
	uint32_t internalStackJobCount = 0;
	size_t terminalStackHighWaterMarkBytes = 0;

	WorkerResult emitResult(WorkerResult result, WorkerJobId jobId = kInvalidJobId) {
		if (!result) {
			emitEvent(WorkerEventType::Error, result.status, jobId, result.message.c_str());
		}
		return result;
	}

	WorkerJobResult emitJobResult(WorkerJobResult result) {
		if (!result) {
			emitEvent(WorkerEventType::Error, result.status, result.jobId, result.message.c_str());
		}
		return result;
	}

	void emitEvent(
	    WorkerEventType type,
	    WorkerStatus status,
	    WorkerJobId jobId,
	    const char *message
	) {
		WorkerEventCallback callback;
		{
			WorkerLock lock(mutex);
			if (!lock) {
				return;
			}
			callback = onEvent;
		}
		if (callback) {
			callback(WorkerEvent{type, status, jobId, message != nullptr ? message : "event"});
		}
	}

	std::shared_ptr<WorkerJobRecord> findJob(WorkerJobId jobId) {
		for (auto &job : jobs) {
			if (job && job->id == jobId) {
				return job;
			}
		}
		return nullptr;
	}

	void resetDiagnostics() {
		totalJobCount = 0;
		finishedJobCount = 0;
		stoppedJobCount = 0;
		failedJobCount = 0;
		psramStackJobCount = 0;
		internalStackJobCount = 0;
		terminalStackHighWaterMarkBytes = 0;
	}

	void accountCreatedJob(const std::shared_ptr<WorkerJobRecord> &job) {
		if (!job) {
			return;
		}
		totalJobCount++;
		if (job->actualStackType == WorkerStackType::Psram) {
			psramStackJobCount++;
		} else {
			internalStackJobCount++;
		}
	}

	void accountTerminalJob(const std::shared_ptr<WorkerJobRecord> &job) {
		if (!job || job->terminalAccounted) {
			return;
		}
		job->terminalAccounted = true;
		switch (job->state) {
		case WorkerJobState::Finished:
			finishedJobCount++;
			break;
		case WorkerJobState::Stopped:
			stoppedJobCount++;
			break;
		case WorkerJobState::Failed:
			failedJobCount++;
			break;
		case WorkerJobState::Created:
		case WorkerJobState::Running:
		case WorkerJobState::Sleeping:
		case WorkerJobState::Stopping:
			break;
		}
		terminalStackHighWaterMarkBytes += job->stackHighWaterMarkBytes;
	}

	void reapJob(const std::shared_ptr<WorkerJobRecord> &job) {
		if (!job) {
			return;
		}
		jobs.erase(
		    std::remove_if(
		        jobs.begin(),
		        jobs.end(),
		        [&](const std::shared_ptr<WorkerJobRecord> &candidate) {
			        return candidate && candidate->id == job->id;
		        }
		    ),
		    jobs.end()
		);
	}

	void reapTerminalJobs() {
		jobs.erase(
		    std::remove_if(
		        jobs.begin(),
		        jobs.end(),
		        [](const std::shared_ptr<WorkerJobRecord> &candidate) {
			        return isReapableJob(candidate);
		        }
		    ),
		    jobs.end()
		);
	}

	void markFailed(const std::shared_ptr<WorkerJobRecord> &job) {
		if (!job) {
			return;
		}
		job->state = WorkerJobState::Failed;
		job->finishedAtMs = nowMs();
		job->taskHandle = nullptr;
		job->taskExited = true;
		accountTerminalJob(job);
	}

	WorkerResult waitForJob(
	    const std::shared_ptr<WorkerJobRecord> &job,
	    WorkerJobId jobId,
	    uint32_t timeoutMs
	) {
		if (!job) {
			return emitResult(
			    WorkerResult::failure(WorkerStatus::JobNotFound, "job not found"),
			    jobId
			);
		}
		const uint32_t startMs = nowMs();
		while (true) {
			{
				WorkerLock lock(mutex);
				if (!lock) {
					return emitResult(
					    WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker"),
					    jobId
					);
				}
				if (isTerminalState(job->state) && job->taskExited) {
					reapJob(job);
					return WorkerResult::success("job finished");
				}
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
		jobConfig.stackType = config.defaultStackType;
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

	void setState(
	    const std::shared_ptr<WorkerJobRecord> &job,
	    WorkerJobState state,
	    bool updateFinishTime = false
	) {
		WorkerLock lock(mutex);
		if (!lock || !job) {
			return;
		}
		job->state = state;
		if (updateFinishTime) {
			job->finishedAtMs = nowMs();
		}
	}

	void markRunStart(const std::shared_ptr<WorkerJobRecord> &job) {
		WorkerLock lock(mutex);
		if (!lock || !job) {
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

	void markTaskFinished(
	    const std::shared_ptr<WorkerJobRecord> &job,
	    WorkerJobState finalState
	) {
		if (!job) {
			return;
		}
		{
			WorkerLock lock(mutex);
			if (lock) {
				job->stackHighWaterMarkBytes =
				    worker_task_support::currentStackHighWaterMarkBytes();
				job->state = finalState;
				job->finishedAtMs = nowMs();
				job->taskHandle = nullptr;
				accountTerminalJob(job);
			}
		}
		if (finalState == WorkerJobState::Stopped) {
			emitEvent(WorkerEventType::Info, WorkerStatus::Ok, job->id, "job stopped");
		} else if (finalState == WorkerJobState::Finished) {
			emitEvent(WorkerEventType::Info, WorkerStatus::Ok, job->id, "job finished");
		} else {
			emitEvent(WorkerEventType::Error, WorkerStatus::InternalError, job->id, "job failed");
		}
	}

	void markTaskExited(WorkerJobId jobId) {
		WorkerLock lock(mutex);
		if (!lock) {
			return;
		}
		auto job = findJob(jobId);
		if (job) {
			job->taskExited = true;
		}
	}

	bool waitWhileSleeping(const std::shared_ptr<WorkerJobRecord> &job, uint32_t durationMs) {
		if (!job || durationMs == 0) {
			return true;
		}
		return waitForDuration(job, durationMs);
	}

	bool waitForDuration(const std::shared_ptr<WorkerJobRecord> &job, uint32_t durationMs) {
		if (!job) {
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
					if (!isTerminalState(job->state)) {
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

	void requestSleep(const std::shared_ptr<WorkerJobRecord> &job, uint32_t durationMs) {
		if (!job || durationMs == 0) {
			return;
		}
		TaskHandle_t handle = nullptr;
		{
			WorkerLock lock(mutex);
			if (!lock || isTerminalState(job->state)) {
				return;
			}
			const uint32_t existingRemainingMs = job->hasSleepDeadline
			    ? remainingSince(job->sleepStartMs, job->sleepDurationMs)
			    : 0;
			job->sleepStartMs = nowMs();
			job->sleepDurationMs = std::max(existingRemainingMs, durationMs);
			job->hasSleepDeadline = true;
			job->state = WorkerJobState::Sleeping;
			handle = job->taskHandle;
		}
		if (handle != nullptr) {
			xTaskNotifyGive(handle);
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

		const WorkerJobConfig jobConfig = resolveJobConfig(incomingConfig);
		if (!worker_task_support::isValidStackSize(jobConfig.stackSize)) {
			return emitJobResult(WorkerJobResult::failure(
			    WorkerStatus::InvalidArgument,
			    "stack size must be at least 1024 bytes and aligned"
			));
		}

		bool usePsramStack = false;
		WorkerStackType actualStackType = WorkerStackType::Internal;
		if (jobConfig.stackType == WorkerStackType::Psram) {
			if (!worker_task_support::hasExternalStackSupport()) {
				return emitJobResult(WorkerJobResult::failure(
				    WorkerStatus::TaskCreateFailed,
				    "PSRAM task stacks are not available"
				));
			}
			usePsramStack = true;
			actualStackType = WorkerStackType::Psram;
		} else if (jobConfig.stackType == WorkerStackType::Auto &&
		           worker_task_support::hasExternalStackSupport()) {
			usePsramStack = true;
			actualStackType = WorkerStackType::Psram;
		}

		std::shared_ptr<WorkerJobRecord> job(new (std::nothrow) WorkerJobRecord());
		if (!job) {
			return emitJobResult(WorkerJobResult::failure(
			    WorkerStatus::OutOfMemory,
			    "failed to allocate job record"
			));
		}

		job->owner = this;
		job->callback = callback;
		job->recurring = recurring;
		job->intervalMs = intervalMs;
		job->stackSize = jobConfig.stackSize;
		job->priority = jobConfig.priority;
		job->coreId = jobConfig.coreId;
		job->requestedStackType = jobConfig.stackType;
		job->actualStackType = actualStackType;
		if (jobConfig.name != nullptr && *jobConfig.name != '\0') {
			copyTaskName(job->name, sizeof(job->name), jobConfig.name);
		}

		auto taskArg = new (std::nothrow) std::shared_ptr<WorkerJobRecord>(job);
		if (taskArg == nullptr) {
			return emitJobResult(WorkerJobResult::failure(
			    WorkerStatus::OutOfMemory,
			    "failed to allocate task argument"
			));
		}

		TaskHandle_t handle = nullptr;
		bool createdWithCaps = false;

		{
			WorkerLock lock(mutex);
			if (!lock) {
				delete taskArg;
				return emitJobResult(WorkerJobResult::failure(
				    WorkerStatus::InternalError,
				    "failed to lock worker registry"
				));
			}
			if (!initialized) {
				delete taskArg;
				return emitJobResult(WorkerJobResult::failure(
				    WorkerStatus::NotInitialized,
				    "worker is not initialized"
				));
			}
			if (ending) {
				delete taskArg;
				return emitJobResult(WorkerJobResult::failure(
				    WorkerStatus::Busy,
				    "worker is ending"
				));
			}

			job->id = nextJobId++;
			accountCreatedJob(job);
			jobs.push_back(job);

			const BaseType_t created = worker_task_support::createTask(
			    &WorkerImpl::taskEntry,
			    job->name,
			    job->stackSize,
			    taskArg,
			    job->priority,
			    &handle,
			    job->coreId,
			    usePsramStack,
			    createdWithCaps
			);
			if (created != pdPASS || handle == nullptr) {
				delete taskArg;
				markFailed(job);
				return emitJobResult(WorkerJobResult::failure(
				    WorkerStatus::TaskCreateFailed,
				    "failed to create job task",
				    job->id
				));
			}
			job->taskHandle = handle;
			job->createdWithCaps = createdWithCaps;
		}

		return WorkerJobResult::success(job->id, "job started");
	}

	static TaskEntryExit runTaskEntry(void *arg) {
		TaskEntryExit exit;
		std::unique_ptr<std::shared_ptr<WorkerJobRecord>> holder(
		    static_cast<std::shared_ptr<WorkerJobRecord> *>(arg)
		);
		if (!holder || !(*holder)) {
			// Defensive only: stack allocation caps are unknowable without a valid task arg.
			return exit;
		}

		auto job = *holder;
		exit.createdWithCaps = job->createdWithCaps;

		WorkerImpl *owner = job->owner;
		if (owner == nullptr) {
			return exit;
		}

		exit.owner = owner;
		exit.jobId = job->id;

		WorkerJobContext context(job);
		WorkerJobState finalState = WorkerJobState::Finished;

		if (job->recurring) {
			while (!job->stopRequested.load()) {
				owner->markRunStart(job);
				job->callback(context);
				if (job->stopRequested.load()) {
					break;
				}
				owner->waitForDuration(job, job->intervalMs);
			}
			finalState = WorkerJobState::Stopped;
		} else {
			owner->markRunStart(job);
			job->callback(context);
			finalState =
			    job->stopRequested.load() ? WorkerJobState::Stopped : WorkerJobState::Finished;
		}

		owner->markTaskFinished(job, finalState);
		return exit;
	}

	static void taskEntry(void *arg) {
		const TaskEntryExit exit = runTaskEntry(arg);
		if (exit.owner != nullptr && exit.jobId != kInvalidJobId) {
			exit.owner->markTaskExited(exit.jobId);
		}
		worker_task_support::deleteCurrentTask(exit.createdWithCaps);
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

WorkerJobContext::WorkerJobContext(std::shared_ptr<WorkerJobRecord> record) : _record(record) {
}

WorkerJobId WorkerJobContext::id() const {
	return _record ? _record->id : kInvalidJobId;
}

void WorkerJobContext::stop() {
	if (_record) {
		_record->stopRequested.store(true);
	}
}

void WorkerJobContext::sleep(uint32_t durationMs) {
	if (!_record || _record->owner == nullptr) {
		return;
	}
	_record->owner->waitWhileSleeping(_record, durationMs);
}

bool WorkerJobContext::shouldStop() const {
	return _record == nullptr || _record->stopRequested.load();
}

uint32_t WorkerJobContext::runCount() const {
	if (!_record || _record->owner == nullptr) {
		return 0;
	}
	WorkerLock lock(_record->owner->mutex);
	return lock ? _record->runCount : 0;
}

uint64_t WorkerJobContext::startedAtMs() const {
	if (!_record || _record->owner == nullptr) {
		return 0;
	}
	WorkerLock lock(_record->owner->mutex);
	return lock ? _record->startedAtMs : 0;
}

uint64_t WorkerJobContext::lastRunAtMs() const {
	if (!_record || _record->owner == nullptr) {
		return 0;
	}
	WorkerLock lock(_record->owner->mutex);
	return lock ? _record->lastRunAtMs : 0;
}

Worker::Worker() : _impl(new (std::nothrow) WorkerImpl()) {
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
	WorkerResult failure;
	bool hasFailure = false;
	{
		WorkerLock lock(_impl->mutex);
		if (!lock) {
			return WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker");
		}
		if (_impl->initialized) {
			failure =
			    WorkerResult::failure(WorkerStatus::AlreadyInitialized, "worker already initialized");
			hasFailure = true;
		} else if (!worker_task_support::isValidStackSize(config.defaultStackSize)) {
			failure = WorkerResult::failure(
			    WorkerStatus::InvalidArgument,
			    "default stack size must be at least 1024 bytes and aligned"
			);
			hasFailure = true;
		} else {
			_impl->config = config;
			_impl->initialized = true;
			_impl->ending = false;
			_impl->nextJobId = 1;
			_impl->jobs.clear();
			_impl->resetDiagnostics();
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
	WorkerLock lock(_impl->mutex);
	if (lock) {
		_impl->onEvent = callback;
	}
}

WorkerJobResult Worker::once(WorkerCallback callback) {
	if (!_impl) {
		return WorkerJobResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	return _impl->startJob(_impl->defaultJobConfig(), callback, false, 0);
}

WorkerJobResult Worker::once(const WorkerJobConfig &config, WorkerCallback callback) {
	if (!_impl) {
		return WorkerJobResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	return _impl->startJob(config, callback, false, 0);
}

WorkerJobResult Worker::every(uint32_t intervalMs, WorkerCallback callback) {
	if (!_impl) {
		return WorkerJobResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	return _impl->startJob(_impl->defaultJobConfig(), callback, true, intervalMs);
}

WorkerJobResult Worker::every(
    uint32_t intervalMs,
    const WorkerJobConfig &config,
    WorkerCallback callback
) {
	if (!_impl) {
		return WorkerJobResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	return _impl->startJob(config, callback, true, intervalMs);
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
			auto job = _impl->findJob(jobId);
			if (!job) {
				failure = WorkerResult::failure(WorkerStatus::JobNotFound, "job not found");
				hasFailure = true;
			} else if (isTerminalState(job->state)) {
				alreadyFinished = true;
			} else {
				job->stopRequested.store(true);
				job->state = WorkerJobState::Stopping;
				handle = job->taskHandle;
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
	std::shared_ptr<WorkerJobRecord> job;
	WorkerResult failure;
	bool hasFailure = false;
	{
		WorkerLock lock(_impl->mutex);
		if (!lock) {
			failure = WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker");
			hasFailure = true;
		} else {
			job = _impl->findJob(jobId);
			if (!job) {
				failure = WorkerResult::failure(WorkerStatus::JobNotFound, "job not found");
				hasFailure = true;
			} else if (!isTerminalState(job->state)) {
				job->stopRequested.store(true);
				job->state = WorkerJobState::Stopping;
				handle = job->taskHandle;
			}
		}
	}
	if (hasFailure) {
		return _impl->emitResult(failure, jobId);
	}
	if (handle != nullptr) {
		xTaskNotifyGive(handle);
	}
	return _impl->waitForJob(job, jobId, timeoutMs);
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
	std::shared_ptr<WorkerJobRecord> job;
	WorkerResult failure;
	bool hasFailure = false;
	{
		WorkerLock lock(_impl->mutex);
		if (!lock) {
			failure = WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker");
			hasFailure = true;
		} else {
			job = _impl->findJob(jobId);
			if (!job) {
				failure = WorkerResult::failure(WorkerStatus::JobNotFound, "job not found");
				hasFailure = true;
			} else if (isTerminalState(job->state)) {
				failure = WorkerResult::failure(WorkerStatus::InvalidArgument, "job already finished");
				hasFailure = true;
			}
		}
	}
	if (hasFailure) {
		return _impl->emitResult(failure, jobId);
	}
	_impl->requestSleep(job, durationMs);
	return WorkerResult::success("job sleep requested");
}

WorkerResult Worker::waitFor(WorkerJobId jobId) {
	return waitFor(jobId, UINT32_MAX);
}

WorkerResult Worker::waitFor(WorkerJobId jobId, uint32_t timeoutMs) {
	if (!_impl) {
		return WorkerResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	std::shared_ptr<WorkerJobRecord> job;
	{
		WorkerLock lock(_impl->mutex);
		if (!lock) {
			return _impl->emitResult(
			    WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker"),
			    jobId
			);
		}
		job = _impl->findJob(jobId);
		if (!job) {
			return _impl->emitResult(
			    WorkerResult::failure(WorkerStatus::JobNotFound, "job not found"),
			    jobId
			);
		}
	}
	return _impl->waitForJob(job, jobId, timeoutMs);
}

WorkerResult Worker::clearFinished() {
	if (!_impl) {
		return WorkerResult::failure(WorkerStatus::OutOfMemory, "failed to allocate worker");
	}
	WorkerLock lock(_impl->mutex);
	if (!lock) {
		return WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker");
	}
	_impl->reapTerminalJobs();
	return WorkerResult::success("finished jobs cleared");
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
	diag.totalJobCount = _impl->totalJobCount;
	diag.finishedJobCount = _impl->finishedJobCount;
	diag.stoppedJobCount = _impl->stoppedJobCount;
	diag.failedJobCount = _impl->failedJobCount;
	diag.psramStackJobCount = _impl->psramStackJobCount;
	diag.internalStackJobCount = _impl->internalStackJobCount;
	diag.totalStackHighWaterMarkBytes = _impl->terminalStackHighWaterMarkBytes;
	for (const auto &job : _impl->jobs) {
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
		case WorkerJobState::Created:
		case WorkerJobState::Stopping:
		case WorkerJobState::Stopped:
		case WorkerJobState::Finished:
		case WorkerJobState::Failed:
			break;
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
			auto job = _impl->findJob(jobId);
			if (!job) {
				failure = WorkerResult::failure(WorkerStatus::JobNotFound, "job not found");
				hasFailure = true;
			} else {
				out.jobId = job->id;
				out.state = job->state;
				out.name = job->name;
				out.stackSize = job->stackSize;
				out.priority = job->priority;
				out.coreId = job->coreId;
				out.requestedStackType = job->requestedStackType;
				out.actualStackType = job->actualStackType;
				out.runCount = job->runCount;
				out.startedAtMs = job->startedAtMs;
				out.lastRunAtMs = job->lastRunAtMs;
				out.finishedAtMs = job->finishedAtMs;
				out.stackHighWaterMarkBytes = job->stackHighWaterMarkBytes;
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
	std::vector<TaskHandle_t> handles;
	{
		WorkerLock lock(_impl->mutex);
		if (!lock) {
			return WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker");
		}
		if (!_impl->initialized) {
			return WorkerResult::success("worker not initialized");
		}
		_impl->ending = true;
		for (auto &job : _impl->jobs) {
			if (!job || isTerminalState(job->state)) {
				continue;
			}
			job->stopRequested.store(true);
			job->state = WorkerJobState::Stopping;
			if (job->taskHandle != nullptr) {
				handles.push_back(job->taskHandle);
			}
		}
	}
	for (TaskHandle_t handle : handles) {
		xTaskNotifyGive(handle);
	}

	const uint32_t startMs = nowMs();
	while (true) {
		bool allFinished = true;
		{
			WorkerLock lock(_impl->mutex);
			if (!lock) {
				return WorkerResult::failure(WorkerStatus::InternalError, "failed to lock worker");
			}
			for (auto &job : _impl->jobs) {
				if (job && !isReapableJob(job)) {
					allFinished = false;
					break;
				}
			}
		}
		if (allFinished) {
			break;
		}
		if (elapsedSince(startMs, timeoutMs)) {
			return _impl->emitResult(
			    WorkerResult::failure(WorkerStatus::Timeout, "worker end timed out")
			);
		}
		vTaskDelay(pdMS_TO_TICKS(kWaitPollMs));
	}

	{
		WorkerLock lock(_impl->mutex);
		if (lock) {
			_impl->jobs.clear();
			_impl->nextJobId = 1;
			_impl->initialized = false;
			_impl->ending = false;
			_impl->resetDiagnostics();
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
	}
	return "unknown";
}
