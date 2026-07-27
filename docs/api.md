# API Reference

This page summarizes the public API declared in `src/Worker.h`.

## Results

Worker does not intentionally throw exceptions. Operations report normal failures through `WorkerResult` or `WorkerJobResult`.

| Field | Meaning |
| --- | --- |
| `result` | `true` on success, `false` on failure. |
| `status` | Machine-readable `WorkerStatus`. |
| `message` | Human-readable status. |
| `jobId` | Returned by `WorkerJobResult` after a job was created. |

`WorkerStatus` values are `Ok`, `NotInitialized`, `AlreadyInitialized`, `InvalidArgument`, `OutOfMemory`, `TaskCreateFailed`, `JobNotFound`, `Busy`, `Timeout`, and `InternalError`.

## Worker

| Method | Purpose |
| --- | --- |
| `init(config)` | Initialize Worker and its cleanup task. |
| `onEvent(callback)` | Register a synchronous event callback. |
| `once(callback)` | Start an automatically cleaned one-off task. |
| `once(config, callback)` | Start a configured automatically cleaned one-off task. |
| `every(intervalMs, callback)` | Start a recurring task with internal delay. |
| `every(intervalMs, config, callback)` | Start a configured recurring task. |
| `stop(jobId)` | Request cooperative stop. Cleanup continues automatically. |
| `stopAndWait(jobId, timeoutMs)` | Request stop and wait until the task stack and TCB are released. |
| `sleep(jobId, durationMs)` | Request that a job sleeps. |
| `waitFor(jobId)` | Optionally wait until physical task cleanup completes. |
| `waitFor(jobId, timeoutMs)` | Wait with timeout until physical task cleanup completes. |
| `clearFinished()` | Deprecated compatibility no-op. Worker cleans jobs automatically. |
| `getDiagnostics()` | Return current runtime and cleanup-task state. |
| `getJobDiagnostics(jobId, out)` | Fill diagnostics for a currently active job. |
| `end(timeoutMs)` | Stop jobs, drain cleanup, and stop Worker infrastructure. |

`once()` and `every()` are safe for fire-and-forget use. A caller never needs `waitFor()` or `clearFinished()` to release Worker-owned resources.

## Cleanup lifecycle

Worker owns every task it creates. A completed job follows this lifecycle:

1. The callback returns and its stored `std::function` is released.
2. The job queues its task handle to the Worker cleanup task.
3. The job task suspends itself.
4. The cleanup task deletes it externally with `vTaskDelete()` or `vTaskDeleteWithCaps()` as appropriate.
5. Worker records a small bounded completion token and removes the full active job record.

`waitFor()` and `stopAndWait()` succeed only after step 4. They are synchronization APIs, not cleanup APIs.

## Events

Register an event callback with `onEvent()`.

```cpp
worker.onEvent([](WorkerEvent event) {
	Serial.printf("Worker event occurred: %s", event.message);
});
```

Completion events are emitted after physical task deletion completes.

## Job context

Callbacks receive `WorkerJobContext&`.

| Method | Purpose |
| --- | --- |
| `id()` | Return the current job ID. |
| `stop()` | Request that the current job stops. |
| `sleep(durationMs)` | Sleep the current job cooperatively. |
| `shouldStop()` | Check the cooperative stop flag. |
| `runCount()` | Number of callback runs started. |
| `startedAtMs()` | First run time from `millis()`. |
| `lastRunAtMs()` | Most recent run time from `millis()`. |

The context is valid only during callback execution.

## Diagnostics

`WorkerDiag` reports current state only:

- active, running, sleeping, stopping, and cleanup-queued job counts;
- cleanup-task running state;
- cleanup queue depth and high-water mark.

Worker does not retain lifetime job counters. `WorkerJobDiag` is available only while a job is active. After automatic cleanup removes the active record, `getJobDiagnostics()` returns `JobNotFound`.

A bounded internal completion window allows `waitFor()` to observe fast jobs after their active records have already been removed. It does not retain callbacks, task handles, names, or full diagnostics.

The Worker destructor performs cooperative shutdown without a timeout so tasks cannot outlive Worker internals.
