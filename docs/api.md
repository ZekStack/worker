# API Reference

This page summarizes the public API declared in `src/Worker.h`.

## Results

Worker does not throw exceptions. Operations return `WorkerResult` or `WorkerJobResult`.

| Field | Meaning |
| --- | --- |
| `result` | `true` on success, `false` on failure. |
| `status` | Machine-readable `WorkerStatus`. |
| `message` | Human-readable status. |
| `jobId` | Returned by `WorkerJobResult` when a job was created or partially allocated. |

`WorkerStatus` values are `Ok`, `NotInitialized`, `AlreadyInitialized`, `InvalidArgument`, `OutOfMemory`, `TaskCreateFailed`, `JobNotFound`, `Busy`, `Timeout`, and `InternalError`.

## Worker

| Method | Purpose |
| --- | --- |
| `init(config)` | Initialize Worker defaults. |
| `onEvent(callback)` | Register a synchronous event callback. |
| `once(callback)` | Start a one-off task. |
| `once(config, callback)` | Start a configured one-off task. |
| `every(intervalMs, callback)` | Start a recurring task with internal delay. |
| `every(intervalMs, config, callback)` | Start a configured recurring task. |
| `stop(jobId)` | Request cooperative stop. |
| `stopAndWait(jobId, timeoutMs)` | Request stop and wait until terminal state. |
| `sleep(jobId, durationMs)` | Request that a job sleeps. |
| `waitFor(jobId)` | Wait until the job reaches a terminal state. |
| `waitFor(jobId, timeoutMs)` | Wait with timeout. |
| `getDiagnostics()` | Return aggregate diagnostics. |
| `getJobDiagnostics(jobId, out)` | Fill per-job diagnostics. |
| `end(timeoutMs)` | Stop jobs and end Worker. |

## Events

Register an event callback with `onEvent()`.

```cpp
worker.onEvent([](WorkerEvent event) {
	Serial.printf("Worker event occurred: %s", event.message);
});
```

`WorkerEvent::type` is the source of truth for event severity. Error events use `WorkerEventType::Error` and include a `WorkerStatus`.

## Job Context

Callbacks receive `WorkerJobContext&`.

| Method | Purpose |
| --- | --- |
| `id()` | Return the current job id. |
| `stop()` | Request that the current job stops. |
| `sleep(durationMs)` | Sleep the current job cooperatively. |
| `shouldStop()` | Check the cooperative stop flag. |
| `runCount()` | Number of callback runs started. |
| `startedAtMs()` | First run time from `millis()`. |
| `lastRunAtMs()` | Most recent run time from `millis()`. |

## Diagnostics

`WorkerDiag` reports aggregate job counts and stack diagnostics. `WorkerJobDiag` reports state, name, stack config, run count, timing, and stack high-water data for one job.

Completed job records stay available for diagnostics until `end()`.
