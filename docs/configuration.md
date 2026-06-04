# Configuration

`WorkerConfig` controls defaults used by jobs created without an explicit `WorkerJobConfig`.

| Field | Default | Meaning |
| --- | --- | --- |
| `defaultStackSize` | `4096` | FreeRTOS task stack size in bytes. |
| `defaultPriority` | `1` | FreeRTOS task priority. |
| `defaultCoreId` | `tskNO_AFFINITY` | Core affinity passed to task creation. |
| `defaultStackType` | `WorkerStackType::Auto` | Stack memory preference. |

`WorkerJobConfig` controls a single job.

| Field | Default | Meaning |
| --- | --- | --- |
| `stackSize` | `0` | `0` uses the worker default. |
| `priority` | `0` | `0` uses the worker default. |
| `coreId` | `tskNO_AFFINITY` | FreeRTOS core affinity. |
| `stackType` | `WorkerStackType::Auto` | `Auto`, `Internal`, or `Psram`. |
| `name` | `nullptr` | Optional FreeRTOS task name. |

Stack sizes are byte counts on ESP32. Worker rejects stack sizes below 1024 bytes or sizes that are not aligned to `sizeof(StackType_t)`.

`WorkerStackType::Auto` uses PSRAM stacks when the ESP-IDF task-capability API and PSRAM are available. It falls back to internal RAM otherwise.

`WorkerStackType::Psram` requires PSRAM task stack support. Job creation fails if it is unavailable.
