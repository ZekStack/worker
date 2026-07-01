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
| `waitFor(jobId)` | Wait until a registered job reaches a terminal state. |
| `waitFor(jobId, timeoutMs)` | Wait with timeout while the job is registered or already being waited on. |
| `getDiagnostics()` | Return aggregate lifetime diagnostics and current active counts. |
| `getJobDiagnostics(jobId, out)` | Fill per-job diagnostics for an active registered job. |
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

`WorkerDiag` reports aggregate job counts and stack diagnostics. `totalJobCount`, `finishedJobCount`, `stoppedJobCount`, `failedJobCount`, stack type counts, and total stack high-water data are lifetime counters since `init()`. `runningJobCount` and `sleepingJobCount` describe currently active registered jobs.

Completed job records are reaped automatically after they reach `Finished`, `Stopped`, or `Failed`. `WorkerJobDiag` reports state, name, stack config, run count, timing, and stack high-water data while a job is still registered. After auto-reap, `getJobDiagnostics()` returns `JobNotFound`.

`waitFor()` succeeds if it begins while the job is still registered. If a completed job has already been reaped before `waitFor()` starts, it returns `JobNotFound`.
