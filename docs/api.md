# API Reference

This page summarizes the public API declared in `src/Worker.h`.

## Results

Worker does not intentionally throw exceptions. Operations report normal failures through `WorkerResult` or `WorkerJobResult`.

Catastrophic STL allocation failure while constructing result messages, callbacks, or internal containers is not recoverable by Worker on platforms where the standard library throws or aborts.

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
| `stopAndWait(jobId, timeoutMs)` | Request stop and wait until terminal state and task cleanup. |
| `sleep(jobId, durationMs)` | Request that a job sleeps. |
| `waitFor(jobId)` | Wait until a registered or retained job reaches terminal state and task cleanup. |
| `waitFor(jobId, timeoutMs)` | Wait with timeout until terminal state and task cleanup. |
| `clearFinished()` | Reap retained terminal job records. |
| `getDiagnostics()` | Return aggregate lifetime diagnostics and current active counts. |
| `getJobDiagnostics(jobId, out)` | Fill per-job diagnostics for an active or retained terminal job. |
| `end(timeoutMs)` | Stop jobs and end Worker, returning `Timeout` if callbacks do not finish in time. |

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

`WorkerDiag` reports aggregate job counts and stack diagnostics. `totalJobCount`, `finishedJobCount`, `stoppedJobCount`, `failedJobCount`, stack type counts, and total stack high-water data are lifetime counters since `init()`. `runningJobCount` and `sleepingJobCount` describe currently active jobs.

Completed job records are retained after they reach `Finished`, `Stopped`, or `Failed`. `WorkerJobDiag` reports state, name, stack config, run count, timing, and stack high-water data while a job is active or retained. After `waitFor()` consumes the job following task cleanup, `clearFinished()` reaps it, or Worker ends, `getJobDiagnostics()` returns `JobNotFound`.

`waitFor()` and `stopAndWait()` return success only after the job has reached a terminal state and its task entry has completed C++ cleanup. Callback captures and task-entry RAII objects have been released before the job is reaped.

The Worker destructor performs the same cooperative shutdown without a timeout so running tasks cannot continue after Worker internals are destroyed.
