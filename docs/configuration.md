# Configuration

`WorkerConfig` controls job defaults and Worker-owned cleanup infrastructure.

| Field | Default | Meaning |
| --- | --- | --- |
| `defaultStackSize` | `4096` | Default job task stack size in bytes. |
| `defaultPriority` | `1` | Default job task priority. |
| `defaultCoreId` | `tskNO_AFFINITY` | Default job core affinity. |
| `defaultStackType` | `WorkerStackType::Auto` | Default stack memory preference. |
| `maxConcurrentJobs` | `8` | Maximum active jobs and cleanup queue capacity. |
| `cleanupTaskStackSize` | `3072` | Internal-RAM cleanup task stack size in bytes. |
| `cleanupTaskPriority` | `1` | Cleanup task priority. |
| `cleanupTaskCoreId` | `tskNO_AFFINITY` | Cleanup task core affinity. |

Worker rejects new jobs with `WorkerStatus::Busy` when `maxConcurrentJobs` is reached. This bound guarantees one cleanup queue slot for every task Worker allows to exist.

`WorkerJobConfig` controls a single job.

| Field | Default | Meaning |
| --- | --- | --- |
| `stackSize` | `0` | `0` uses the Worker default. |
| `priority` | `0` | `0` uses the Worker default. |
| `coreId` | `tskNO_AFFINITY` | FreeRTOS core affinity. |
| `stackType` | `WorkerStackType::Auto` | `Auto`, `Internal`, or `Psram`. |
| `name` | `nullptr` | Optional task name copied into fixed Worker storage. |

Stack sizes are byte counts on ESP32. Worker rejects stack sizes below 1024 bytes or sizes that are not aligned to `sizeof(StackType_t)`.

`WorkerStackType::Auto` uses PSRAM stacks when ESP-IDF task-capability support and PSRAM are available. It falls back to internal RAM otherwise.

`WorkerStackType::Psram` requires PSRAM task stack support. Job creation fails if it is unavailable. Worker always deletes a capability-created task externally with `vTaskDeleteWithCaps()`.
