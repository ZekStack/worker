# Examples

## Basic

Shows initialization, a fire-and-forget one-off job, a recurring job, optional waiting, and cooperative stop.

## JobConfig

Shows stack size, priority, core affinity, internal stack selection, and PSRAM stack requests.

## Events

Shows synchronous Worker event reporting. Job completion events are emitted after physical task cleanup.

## SleepAndWait

Shows `ctx.sleep()`, external `worker.sleep(jobId, durationMs)`, optional `waitFor()`, and timeout behavior.

## Diagnostics

Shows current active-job counts, cleanup queue state, cleanup-task health, and active per-job diagnostics.

## BindableCallbacks

Shows `std::bind` with private class methods so application classes can own job behavior.

## TaskCleanupSentinel

Runs fire-and-forget one-shot jobs and verifies callback capture destruction, active record cleanup, internal heap stability, PSRAM stability, and task-count recovery after allocator warm-up.
