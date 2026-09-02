# Configuration

`WorkerConfig` controls job defaults, Worker-owned cleanup infrastructure, and the shared ZekStack `Strata::MemoryPolicy`.

## Memory policy

```cpp
WorkerConfig config;
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;
```

Worker's defaults are:

```cpp
allocation = Strata::Placement::Default;
taskStack  = Strata::Placement::PreferExternal;
```

This preserves the pre-`v0.2.0` `WorkerStackType::Auto` behavior for task stacks while adopting the common Strata vocabulary.

`memory.allocation` applies to ordinary movable Worker-owned storage: job records, registry/completion container backing, and cleanup-queue item storage. FreeRTOS control blocks and mutex metadata remain internal through Strata because safety requirements override caller preference.

`memory.taskStack` is the default placement inherited by job stacks and the cleanup-task stack. `PreferExternal` may fall back to internal memory; `RequireExternal` fails if external memory cannot satisfy the request.

## WorkerConfig

| Field | Default | Meaning |
| --- | --- | --- |
| `memory.allocation` | `Strata::Placement::Default` | Default placement for movable Worker-owned allocations. |
| `memory.taskStack` | `Strata::Placement::PreferExternal` | Default placement for Worker-owned task stacks. |
| `defaultStackSize` | `4096` | Default job task stack size in bytes. |
| `defaultPriority` | `1` | Default job task priority. |
| `defaultCoreId` | `tskNO_AFFINITY` | Default job core affinity for overloads that use Worker defaults. |
| `maxConcurrentJobs` | `8` | Maximum active jobs and cleanup queue capacity. |
| `cleanupTaskStackSize` | `3072` | Cleanup task stack size in bytes. |
| `cleanupTaskPriority` | `1` | Cleanup task priority. |
| `cleanupTaskCoreId` | `tskNO_AFFINITY` | Cleanup task core affinity. |
| `cleanupTaskStackPlacement` | `std::nullopt` | Optional cleanup-stack override; `nullopt` inherits `memory.taskStack`. |

Worker rejects new jobs with `WorkerStatus::Busy` when `maxConcurrentJobs` is reached. This bound guarantees one cleanup queue slot for every task Worker allows to exist.

## WorkerJobConfig

| Field | Default | Meaning |
| --- | --- | --- |
| `stackSize` | `0` | `0` uses the Worker default. |
| `priority` | `0` | `0` uses the Worker default. |
| `coreId` | `tskNO_AFFINITY` | FreeRTOS core affinity. |
| `stackPlacement` | `std::nullopt` | Optional Strata placement override. `nullopt` inherits `WorkerConfig::memory.taskStack`. |
| `name` | `nullptr` | Optional task name copied into fixed Worker storage. |

`Placement::Default` never means inherit. If `stackPlacement` contains `Strata::Placement::Default`, Worker explicitly asks Strata for backend-default placement. Inheritance is represented only by `std::nullopt`.

Stack sizes remain byte counts on ESP32. Worker preserves the existing contract and rejects stack sizes below 1024 bytes or values not aligned to `sizeof(StackType_t)`.

## Migration from v0.1.0

| v0.1.0 | v0.2.0 |
| --- | --- |
| `WorkerStackType::Auto` | `Strata::Placement::PreferExternal` |
| `WorkerStackType::Internal` | `Strata::Placement::Internal` |
| `WorkerStackType::Psram` | `Strata::Placement::RequireExternal` |
| `WorkerConfig::defaultStackType` | `WorkerConfig::memory.taskStack` |
| `WorkerJobConfig::stackType` | `WorkerJobConfig::stackPlacement` |

`WorkerStackType` is intentionally removed rather than retained as a compatibility alias so Worker establishes the same configuration vocabulary later ZekStack migrations will use.
