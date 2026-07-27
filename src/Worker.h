#pragma once

#include <Arduino.h>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

using WorkerJobId = uint32_t;

class WorkerJobContext;
struct WorkerEvent;
struct WorkerImpl;
struct WorkerJobRecord;

using WorkerCallback = std::function<void(WorkerJobContext &)>;
using WorkerEventCallback = std::function<void(WorkerEvent)>;

enum class WorkerStatus : uint8_t {
	Ok,
	NotInitialized,
	AlreadyInitialized,
	InvalidArgument,
	OutOfMemory,
	TaskCreateFailed,
	JobNotFound,
	Busy,
	Timeout,
	InternalError,
};

enum class WorkerStackType : uint8_t {
	Auto,
	Internal,
	Psram,
};

enum class WorkerEventType : uint8_t {
	Info,
	Warning,
	Error,
};

enum class WorkerJobState : uint8_t {
	Created,
	Running,
	Sleeping,
	Stopping,
	Stopped,
	Finished,
	Failed,
	CallbackComplete,
	CleanupQueued,
	CleanupComplete,
};

struct WorkerEvent {
	WorkerEventType type = WorkerEventType::Info;
	WorkerStatus status = WorkerStatus::Ok;
	WorkerJobId jobId = 0;
	const char *message = "ok";

	bool isError() const {
		return type == WorkerEventType::Error;
	}
};

struct WorkerConfig {
	uint32_t defaultStackSize = 4096;
	UBaseType_t defaultPriority = 1;
	BaseType_t defaultCoreId = tskNO_AFFINITY;
	WorkerStackType defaultStackType = WorkerStackType::Auto;

	size_t maxConcurrentJobs = 8;
	uint32_t cleanupTaskStackSize = 3072;
	UBaseType_t cleanupTaskPriority = 1;
	BaseType_t cleanupTaskCoreId = tskNO_AFFINITY;
};

struct WorkerJobConfig {
	uint32_t stackSize = 0;
	UBaseType_t priority = 0;
	BaseType_t coreId = tskNO_AFFINITY;
	WorkerStackType stackType = WorkerStackType::Auto;
	const char *name = nullptr;
};

struct WorkerResult {
	bool result = false;
	WorkerStatus status = WorkerStatus::InternalError;
	std::string message;

	explicit operator bool() const {
		return result;
	}

	static WorkerResult success(const char *message = "ok");
	static WorkerResult failure(WorkerStatus status, const char *message);
};

struct WorkerJobResult : WorkerResult {
	WorkerJobId jobId = 0;

	static WorkerJobResult success(WorkerJobId jobId, const char *message = "ok");
	static WorkerJobResult failure(WorkerStatus status, const char *message, WorkerJobId jobId = 0);
};

struct WorkerDiag {
	uint32_t activeJobCount = 0;
	uint32_t runningJobCount = 0;
	uint32_t sleepingJobCount = 0;
	uint32_t stoppingJobCount = 0;
	uint32_t cleanupQueuedCount = 0;
	bool cleanupTaskRunning = false;
	uint32_t cleanupQueueDepth = 0;
	uint32_t cleanupQueueHighWaterMark = 0;
};

struct WorkerJobDiag {
	WorkerJobId jobId = 0;
	WorkerJobState state = WorkerJobState::Created;
	const char *name = nullptr;
	uint32_t stackSize = 0;
	UBaseType_t priority = 0;
	BaseType_t coreId = tskNO_AFFINITY;
	WorkerStackType requestedStackType = WorkerStackType::Auto;
	WorkerStackType actualStackType = WorkerStackType::Internal;
	uint32_t runCount = 0;
	uint64_t startedAtMs = 0;
	uint64_t lastRunAtMs = 0;
	uint64_t finishedAtMs = 0;
	size_t stackHighWaterMarkBytes = 0;
};

class WorkerJobContext {
  public:
	WorkerJobContext() = default;

	WorkerJobId id() const;
	void stop();
	void sleep(uint32_t durationMs);
	bool shouldStop() const;
	uint32_t runCount() const;
	uint64_t startedAtMs() const;
	uint64_t lastRunAtMs() const;

  private:
	friend class Worker;
	friend struct WorkerImpl;

	explicit WorkerJobContext(WorkerJobRecord *record);

	WorkerJobRecord *_record = nullptr;
};

class Worker {
  public:
	Worker();
	~Worker();

	Worker(const Worker &) = delete;
	Worker &operator=(const Worker &) = delete;

	WorkerResult init(const WorkerConfig &config = WorkerConfig());
	void onEvent(WorkerEventCallback callback);

	WorkerJobResult once(WorkerCallback callback);
	WorkerJobResult once(const WorkerJobConfig &config, WorkerCallback callback);

	WorkerJobResult every(uint32_t intervalMs, WorkerCallback callback);
	WorkerJobResult every(
	    uint32_t intervalMs,
	    const WorkerJobConfig &config,
	    WorkerCallback callback
	);

	WorkerResult stop(WorkerJobId jobId);
	WorkerResult stopAndWait(WorkerJobId jobId, uint32_t timeoutMs);

	WorkerResult sleep(WorkerJobId jobId, uint32_t durationMs);
	WorkerResult waitFor(WorkerJobId jobId);
	WorkerResult waitFor(WorkerJobId jobId, uint32_t timeoutMs);

	[[deprecated("Worker cleans completed jobs automatically")]]
	WorkerResult clearFinished();

	WorkerDiag getDiagnostics();
	WorkerResult getJobDiagnostics(WorkerJobId jobId, WorkerJobDiag &out);

	WorkerResult end(uint32_t timeoutMs = 5000);

	const char *statusToString(WorkerStatus status) const;
	const char *eventTypeToString(WorkerEventType type) const;
	const char *jobStateToString(WorkerJobState state) const;

  private:
	std::unique_ptr<WorkerImpl> _impl;
};
