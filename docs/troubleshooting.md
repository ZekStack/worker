# Troubleshooting

## Job creation fails

Check `WorkerJobResult::message` and `status`.

Common causes:

* Worker was not initialized.
* The callback is empty.
* Stack size is below 1024 bytes.
* Stack size is not aligned to `sizeof(StackType_t)`.
* `WorkerStackType::Psram` was requested on a board or framework build without PSRAM task stack support.

## `stopAndWait()` times out

Worker uses cooperative cancellation. `stop()` sets a flag and wakes sleeping jobs, but a callback that blocks forever must return before the task can finish.

Check `ctx.shouldStop()` inside long-running callbacks.

## `every()` runs slower than expected

The interval delay starts after the callback returns. A callback that takes 200 ms with `every(1000, ...)` runs roughly every 1200 ms.

## Stack diagnostics are zero

Some FreeRTOS configurations do not expose stack high-water mark support. Worker reports `0` in that case.
