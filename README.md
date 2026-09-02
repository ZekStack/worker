# Worker

Worker is a FreeRTOS task and cooperative job execution library for ESP32.

Worker runs one-off and recurring background work with explicit task configuration, cooperative stop and sleep controls, event reporting, runtime diagnostics, and automatic task cleanup. Worker owns job orchestration and lifecycle policy while [Strata](https://github.com/ZekStack/strata) owns memory placement and low-level FreeRTOS storage.

[![CI](https://github.com/ZekStack/worker/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/worker/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/worker?sort=semver)](https://github.com/ZekStack/worker/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Why use Worker?

- **Task-per-job execution** — each `once()` and `every()` job owns a FreeRTOS task.
- **Automatic cleanup** — callers never need to reap completed jobs.
- **Consistent memory policy** — `Strata::MemoryPolicy` controls ordinary Worker allocations and task-stack placement.
- **Portable placement** — use `Default`, `Internal`, `PreferExternal`, and `RequireExternal` instead of Worker-specific PSRAM enums.
- **Strata-owned FreeRTOS storage** — task stacks, task control blocks, cleanup queue storage, and mutex control storage use Strata ownership primitives.
- **Safe recurring jobs** — `every()` applies the interval after each callback.
- **Cooperative lifecycle** — jobs can stop or sleep through `WorkerJobContext`.
- **Runtime visibility** — diagnostics expose requested stack placement and observed memory regions.

## Dependency

Worker `v0.2.0` requires Strata `v0.1.1`.

### PlatformIO

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

lib_deps =
    https://github.com/ZekStack/worker.git

build_flags =
    -std=gnu++20
build_unflags =
    -std=gnu++11
```

Worker's `library.json` pins Strata `v0.1.1`, so PlatformIO resolves it as a transitive dependency.

### Arduino IDE

Worker and Strata are not published to Arduino Library Manager yet. Install both repositories into the Arduino libraries directory:

```text
Arduino/libraries/Strata
Arduino/libraries/Worker
```

Use Strata `v0.1.1` or a compatible later release.

## Quick start

```cpp
#include <Arduino.h>
#include <Worker.h>

Worker worker;
WorkerJobId recurringJob = 0;

void setup() {
    Serial.begin(115200);

    WorkerResult initResult = worker.init();
    if (!initResult) {
        Serial.println(initResult.message);
        return;
    }

    worker.once([](WorkerJobContext &ctx) {
        Serial.printf("one-off job id=%u\n", static_cast<unsigned>(ctx.id()));
    });

    WorkerJobResult result = worker.every(1000, [](WorkerJobContext &ctx) {
        Serial.printf("run=%u\n", static_cast<unsigned>(ctx.runCount()));
        if (ctx.runCount() >= 5) {
            ctx.stop();
        }
    });

    if (result) {
        recurringJob = result.jobId;
    }
}

void loop() {
    delay(1000);
}
```

No cleanup call is required after `once()` or `every()`. Worker releases callback captures, Strata task stacks and TCBs, and active job records automatically.

## Memory policy

Worker uses the ZekStack-standard `Strata::MemoryPolicy` configuration shape:

```cpp
WorkerConfig config;
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;

Worker worker;
worker.init(config);
```

`memory.allocation` controls movable Worker-owned storage such as job records, registry/completion container backing, and cleanup-queue item storage. `memory.taskStack` is the inherited default for job and cleanup task stacks.

Worker's default policy preserves the old `WorkerStackType::Auto` behavior:

```cpp
allocation = Strata::Placement::Default;
taskStack  = Strata::Placement::PreferExternal;
```

`PreferExternal` falls back to internal memory when external memory is unavailable. `RequireExternal` fails instead of consuming internal memory.

A job can override only its own stack placement:

```cpp
WorkerJobConfig job;
job.stackPlacement = Strata::Placement::Internal;
worker.once(job, [](WorkerJobContext &) {});
```

`std::nullopt` means inherit `WorkerConfig::memory.taskStack`. `Strata::Placement::Default` never means inherit; it explicitly requests the Strata backend default.

The cleanup task can be overridden independently when needed:

```cpp
config.cleanupTaskStackPlacement = Strata::Placement::Internal;
```

## Cleanup model

Worker creates one long-lived cleanup task during `init()` using `Strata::FreeRTOS::Task` and a task-only `Strata::FreeRTOS::Queue`.

When a job callback finishes, the job:

1. releases its stored callback;
2. records its final state and stack high-water mark;
3. queues its job record to the cleanup task;
4. reaches the external-deletion handoff and suspends.

The cleanup task then externally resets the job's `Strata::FreeRTOS::Task`. Strata deletes the FreeRTOS task and releases its placed stack and internal task control block. Worker records the completion token and removes the active job record only after that reset returns.

`waitFor()` and `stopAndWait()` are optional synchronization APIs. They wait for physical cleanup; they do not perform cleanup.

## Important notes

> [!IMPORTANT]
> Worker cancellation is cooperative. `stop()` requests that a job stops and wakes it if it is sleeping, but it does not interrupt a running callback.

- A callback that blocks forever prevents timed `stopAndWait()` and `end()` calls from completing.
- The destructor waits without a timeout so tasks cannot outlive Worker internals.
- `every(intervalMs, callback)` delays after each callback.
- Stack sizes remain FreeRTOS byte sizes on ESP32, must be at least 1024 bytes, and must be aligned to `sizeof(StackType_t)`.
- `maxConcurrentJobs` bounds active jobs and guarantees cleanup queue capacity.
- `clearFinished()` is retained only as a deprecated compatibility no-op.
- Completion synchronization uses a small bounded token window and never retains callbacks or full completed records.
- `WorkerResult::message` is a non-owning static status string in `v0.2.0`; use `result.message` directly.
- Worker no longer contains direct PSRAM allocation logic, capability-created task handling, or dynamic FreeRTOS queue/mutex creation.
- `std::function` remains the callback surface. Allocation performed by a caller while constructing a callback is outside Worker's owned allocation boundary.

## Diagnostics

`WorkerJobDiag` separates requested policy from actual storage:

```cpp
WorkerJobDiag diag;
if (worker.getJobDiagnostics(jobId, diag)) {
    Serial.printf(
        "requested=%s actual=%s\n",
        Strata::toString(diag.requestedStackPlacement),
        Strata::toString(diag.stackRegion));
}
```

`WorkerDiag` also reports cleanup-task stack placement/region and cleanup-queue storage placement/region so applications can verify memory policy at runtime.

## Examples

| Example | Description |
| --- | --- |
| `Basic` | Minimal initialization, one-off job, recurring job, wait, and cooperative stop. |
| `JobConfig` | Worker memory policy and per-job internal/required-external stack overrides. |
| `Events` | Event callback and error event handling. |
| `SleepAndWait` | Context sleep, external sleep, wait, and timeout behavior. |
| `Diagnostics` | Requested Strata placement and observed stack/cleanup regions. |
| `BindableCallbacks` | `std::bind` with private class methods. |
| `TaskCleanupSentinel` | Fire-and-forget capture, internal/external heap, and task-count cleanup checks under `PreferExternal`. |

Start with:

```text
examples/Basic
```

## Documentation

| Document | Description |
| --- | --- |
| [`docs/getting-started.md`](docs/getting-started.md) | Setup, Strata dependency, and first jobs. |
| [`docs/configuration.md`](docs/configuration.md) | Worker memory policy, job defaults, and cleanup infrastructure. |
| [`docs/api.md`](docs/api.md) | Public API, placement diagnostics, and cleanup semantics. |
| [`docs/examples.md`](docs/examples.md) | Example descriptions. |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | Common lifecycle, placement, and configuration issues. |

## API overview

```cpp
WorkerConfig config;
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;

Worker worker;
worker.init(config);
worker.onEvent([](WorkerEvent event) {});

WorkerJobConfig jobConfig;
jobConfig.stackPlacement = Strata::Placement::Internal;

WorkerJobResult once = worker.once(jobConfig, [](WorkerJobContext &ctx) {});
WorkerJobResult loop = worker.every(1000, [](WorkerJobContext &ctx) {});

worker.sleep(loop.jobId, 5000);
worker.stopAndWait(loop.jobId, 2000);

WorkerDiag diag = worker.getDiagnostics();
WorkerJobDiag jobDiag;
worker.getJobDiagnostics(loop.jobId, jobDiag);
```

## Compatibility

| Item | Support |
| --- | --- |
| Framework | Arduino ESP32 |
| Platform | `espressif32` / PIOArduino |
| Language | C++20 |
| Memory layer | Strata `v0.1.1` |
| External memory | Optional through Strata placement policies |
| Dependencies | Strata `v0.1.1` |
| Exceptions | Not intentionally used by Worker APIs |
| Status | `v0.2.0` API |

## License

MIT — see [`LICENSE.md`](LICENSE.md).

## ZekStack

Part of the ZekStack ESP32 library stack. Worker is the reference adoption of the shared Strata memory-policy contract for higher-level ZekStack libraries.
