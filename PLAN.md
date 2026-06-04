# Worker

Worker is a FreeRTOS task and cooperative job execution library for ESP32.

## v1 behavior

Worker uses one FreeRTOS task per job.

Jobs are cooperative:

* `stop()` requests stop and wakes a sleeping task.
* `stopAndWait()` waits for the callback to return.
* Worker never force-deletes a running callback.
* `every(intervalMs, callback)` owns the delay internally after every callback run.

Both `once()` and `every()` callbacks receive `WorkerJobContext&`.

## Core API

```cpp
WorkerResult init(const WorkerConfig& config = WorkerConfig());
void onEvent(WorkerEventCallback callback);

WorkerJobResult once(WorkerCallback callback);
WorkerJobResult once(const WorkerJobConfig& config, WorkerCallback callback);

WorkerJobResult every(uint32_t intervalMs, WorkerCallback callback);
WorkerJobResult every(uint32_t intervalMs, const WorkerJobConfig& config, WorkerCallback callback);

WorkerResult stop(WorkerJobId jobId);
WorkerResult stopAndWait(WorkerJobId jobId, uint32_t timeoutMs);

WorkerResult sleep(WorkerJobId jobId, uint32_t durationMs);
WorkerResult waitFor(WorkerJobId jobId);
WorkerResult waitFor(WorkerJobId jobId, uint32_t timeoutMs);

WorkerDiag getDiagnostics();
WorkerResult getJobDiagnostics(WorkerJobId jobId, WorkerJobDiag& out);

WorkerResult end(uint32_t timeoutMs = 5000);
```

## Events

Worker emits synchronous events through `onEvent()`.

```cpp
worker.onEvent([](WorkerEvent event) {
	Serial.printf("Worker event occurred: %s", event.message);
});
```

`WorkerEvent::type` clearly separates `Info`, `Warning`, and `Error`.

Error events are emitted for invalid arguments, missing jobs, task creation failure, unavailable PSRAM task stacks, timeouts, and internal failures.

## Diagnostics

`WorkerDiag` reports aggregate job counts and stack usage.

`WorkerJobDiag` reports one job's state, name, stack configuration, run count, timestamps, and stack high-water mark.

Completed job records remain available for diagnostics until `end()` succeeds.

## Stack policy

Stack sizes are ESP32 FreeRTOS byte sizes.

`WorkerStackType`:

* `Auto` prefers PSRAM task stacks when supported and falls back to internal RAM.
* `Internal` forces normal task creation.
* `Psram` requires PSRAM task-stack support and fails clearly when unavailable.
