# API Reference

This page summarizes the public API declared in `src/Worker.h` for Worker `v0.2.0`.

## Results

Worker does not intentionally throw exceptions. Operations report normal failures through `WorkerResult` or `WorkerJobResult`.

| Field | Meaning |
| --- | --- |
| `result` | `true` on success, `false` on failure. |
| `status` | Machine-readable `WorkerStatus`. |
| `message` | Non-owning static human-readable status string. |
| `jobId` | Returned by `WorkerJobResult` after a job was created. |

Use `result.message` directly. `message` is no longer a `std::string` in `v0.2.0`.

`WorkerStatus` values are `Ok`, `NotInitialized`, `AlreadyInitialized`, `InvalidArgument`, `OutOfMemory`, `TaskCreateFailed`, `JobNotFound`, `Busy`, `Timeout`, and `InternalError`.

## Worker

| Method | Purpose |
| --- | --- |
| `init(config)` | Initialize Worker, Strata-backed storage, cleanup queue, and cleanup task. |
| `onEvent(callback)` | Register a synchronous event callback. |
| `once(callback)` | Start an automatically cleaned one-off task. |
| `once(config, callback)` | Start a configured automatically cleaned one-off task. |
| `every(intervalMs, callback)` | Start a recurring task with internal delay. |
| `every(intervalMs, config, callback)` | Start a configured recurring task. |
| `stop(jobId)` | Request cooperative stop. Cleanup continues automatically. |
| `stopAndWait(jobId, timeoutMs)` | Request stop and wait until the Strata task owner releases its stack and TCB. |
| `sleep(jobId, durationMs)` | Request that a job sleeps. |
| `waitFor(jobId)` | Optionally wait until physical task cleanup completes. |
| `waitFor(jobId, timeoutMs)` | Wait with timeout until physical task cleanup completes. |
| `clearFinished()` | Deprecated compatibility no-op. Worker cleans jobs automatically. |
| `getDiagnostics()` | Return current runtime, cleanup infrastructure, and memory-region state. |
| `getJobDiagnostics(jobId, out)` | Fill diagnostics for a currently active job. |
| `end(timeoutMs)` | Stop jobs, drain cleanup, and stop Worker infrastructure. |

`once()` and `every()` are safe for fire-and-forget use. A caller never needs `waitFor()` or `clearFinished()` to release Worker-owned resources.

## Memory configuration

`WorkerConfig` embeds `Strata::MemoryPolicy`:

```cpp
WorkerConfig config;
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;
```

`WorkerJobConfig::stackPlacement` is `std::optional<Strata::Placement>`. `std::nullopt` inherits the Worker task-stack policy. An explicit `Placement::Default` asks Strata for backend-default placement and is not an inheritance sentinel.

The cleanup task can use `WorkerConfig::cleanupTaskStackPlacement` as an optional override; otherwise it also inherits `memory.taskStack`.

## Cleanup lifecycle

Worker owns every job task through `Strata::FreeRTOS::Task`. A completed job follows this lifecycle:

1. The callback returns and its stored `std::function` is released.
2. Worker records final state and stack high-water mark.
3. The job queues its record through `Strata::FreeRTOS::Queue`.
4. The job reaches the external-deletion handoff and suspends.
5. The cleanup task externally resets the job's `Strata::FreeRTOS::Task`.
6. Strata deletes the FreeRTOS task and releases the placed stack and internal task control block.
7. Worker records a small bounded completion token and removes the full active job record.

`waitFor()` and `stopAndWait()` succeed only after the Strata task reset has completed. They are synchronization APIs, not cleanup APIs.

The cleanup task itself follows the same ownership rule: `end()` waits for its handoff, externally resets its Strata task owner, and then resets the Strata cleanup queue.

## Events

Register an event callback with `onEvent()`.

```cpp
worker.onEvent([](WorkerEvent event) {
    Serial.printf("Worker event occurred: %s", event.message);
});
```

Worker stores the callback behind Strata-backed shared ownership so event dispatch can snapshot ownership without copying the `std::function` target while holding the Worker mutex.

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
- cleanup queue depth and high-water mark;
- `cleanupTaskStackPlacement` and observed `cleanupTaskStackRegion`;
- `cleanupQueueStoragePlacement` and observed `cleanupQueueStorageRegion`.

`WorkerJobDiag` reports:

- current job identity/state/name;
- stack size, priority, and affinity;
- `requestedStackPlacement` as the resolved Strata placement intent;
- `stackRegion` as the observed Strata memory region;
- run/timing counters and stack high-water mark.

Requested placement and observed region are intentionally different. A `PreferExternal` stack may legally report `Region::Internal` after fallback.

Worker does not retain lifetime job counters. `WorkerJobDiag` is available only while a job is active. After automatic cleanup removes the active record, `getJobDiagnostics()` returns `JobNotFound`.

A bounded internal completion window allows `waitFor()` to observe fast jobs after their active records have already been removed. It does not retain callbacks, task handles, names, or full diagnostics.

The Worker destructor performs cooperative shutdown without a timeout so tasks cannot outlive Worker internals.
