# Examples

## Basic

Shows initialization, a one-off job, a recurring job, `waitFor()`, and cooperative stop from inside the callback.

## JobConfig

Shows `WorkerJobConfig` with explicit stack size, priority, core affinity, task name, and stack memory preference.

## Events

Shows `onEvent()` and how to check `event.isError()` or `event.type`.

## SleepAndWait

Shows `ctx.sleep()`, external `worker.sleep(jobId, durationMs)`, `waitFor()`, and `stopAndWait()`.

## Diagnostics

Shows aggregate `getDiagnostics()` counters and `getJobDiagnostics()` for an active registered job. Completed jobs are reaped automatically, so per-job diagnostics return `JobNotFound` after the job reaches a terminal state.

## BindableCallbacks

Shows `std::bind` with private class methods, so application classes can own job behavior.
