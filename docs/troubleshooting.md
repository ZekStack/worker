# Troubleshooting

## Job creation fails

Check `WorkerJobResult::message` and `status`.

Common causes:

- Worker was not initialized.
- The callback is empty.
- Stack size is below 1024 bytes or is not aligned to `sizeof(StackType_t)`.
- `Strata::Placement::RequireExternal` was requested but external memory is unavailable or cannot satisfy the task stack.
- `maxConcurrentJobs` was reached. Retry later or raise the configured bound.
- Worker cleanup infrastructure could not be created during `init()`.

Use `PreferExternal` when internal fallback is acceptable. Use `RequireExternal` only when failure is preferable to consuming internal memory.

## `init()` fails after changing memory policy

`WorkerConfig::memory` and `cleanupTaskStackPlacement` are validated by Worker before infrastructure creation.

If `memory.allocation = RequireExternal`, the cleanup queue item storage must be allocated externally. If `memory.taskStack = RequireExternal`, both normal job stacks and the cleanup-task stack inherit that strict requirement unless overridden.

For flash/cache-sensitive application work, explicitly use `Strata::Placement::Internal` for the relevant job stack.

## Requested placement differs from observed region

This is expected for `PreferExternal`. Inspect `WorkerJobDiag::requestedStackPlacement` and `WorkerJobDiag::stackRegion` separately.

`PreferExternal` may report `Region::Internal` after fallback. `RequireExternal` never falls back.

`WorkerDiag` exposes the same requested/observed split for cleanup-task stack and cleanup-queue storage.

## `stopAndWait()` or `end()` times out

Cancellation is cooperative. Worker wakes sleeping jobs, but a callback must return before its task can be cleaned. Check `ctx.shouldStop()` inside long-running callbacks.

`waitFor()` and `stopAndWait()` wait for external Strata task reset, including stack and TCB release. They may take slightly longer than callback completion.

## Active jobs never return to zero

Inspect `cleanupQueuedCount`, `cleanupQueueDepth`, and `cleanupTaskRunning`.

- A nonzero cleanup queue should drain automatically.
- `cleanupTaskRunning` must remain true while Worker is initialized.
- A callback that never returns prevents its job from reaching cleanup.

Callers must not invoke `clearFinished()` for recovery. It is a deprecated no-op because cleanup is Worker-owned.

## `every()` runs slower than expected

The interval starts after the callback returns. A 200 ms callback with `every(1000, ...)` runs roughly every 1200 ms.

## Stack diagnostics are zero

Worker uses Strata's FreeRTOS task high-water-mark API. A zero value can mean the task has already left the active diagnostics window or no usable measurement was available at the observation point.
