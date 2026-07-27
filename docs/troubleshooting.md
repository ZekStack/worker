# Troubleshooting

## Job creation fails

Check `WorkerJobResult::message` and `status`.

Common causes:

- Worker was not initialized.
- The callback is empty.
- Stack size is below 1024 bytes or is not aligned.
- `WorkerStackType::Psram` was requested without PSRAM task stack support.
- `maxConcurrentJobs` was reached. Retry later or raise the configured bound.
- Worker cleanup infrastructure could not be created during `init()`.

## `stopAndWait()` or `end()` times out

Cancellation is cooperative. Worker wakes sleeping jobs, but a callback must return before its task can be cleaned. Check `ctx.shouldStop()` inside long-running callbacks.

`waitFor()` and `stopAndWait()` wait for external task deletion, including stack and TCB release. They may take slightly longer than callback completion.

## Active jobs never return to zero

Inspect `cleanupQueuedCount`, `cleanupQueueDepth`, and `cleanupTaskRunning`.

- A nonzero cleanup queue should drain automatically.
- `cleanupTaskRunning` must remain true while Worker is initialized.
- A callback that never returns prevents its job from reaching cleanup.

Callers must not invoke `clearFinished()` for recovery. It is a deprecated no-op because cleanup is Worker-owned.

## `every()` runs slower than expected

The interval starts after the callback returns. A 200 ms callback with `every(1000, ...)` runs roughly every 1200 ms.

## Stack diagnostics are zero

Some FreeRTOS configurations do not expose stack high-water mark support. Worker reports `0` in that case.
