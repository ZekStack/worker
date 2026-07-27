# Worker

Worker is a FreeRTOS task and cooperative job execution library for ESP32.

Worker runs one-off and recurring background work with explicit task configuration, cooperative stop and sleep controls, event reporting, runtime diagnostics, and automatic task cleanup. It is designed for products that need predictable task behavior without spreading raw FreeRTOS task management across the application.

[![CI](https://github.com/ZekStack/worker/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/worker/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/worker?sort=semver)](https://github.com/ZekStack/worker/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Why use Worker?

- **Task-per-job execution** — each `once()` and `every()` job owns a FreeRTOS task.
- **Automatic cleanup** — callers never need to reap completed jobs.
- **Correct PSRAM teardown** — capability-created tasks are externally deleted with `vTaskDeleteWithCaps()`.
- **Safe recurring jobs** — `every()` applies the interval after each callback.
- **ESP32 task control** — configure byte stack size, priority, core affinity, and stack memory preference.
- **Cooperative lifecycle** — jobs can stop or sleep through `WorkerJobContext`.
- **Runtime visibility** — current job and cleanup-task diagnostics without retained job history.

## Install

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

### Arduino IDE

Worker is not published to Arduino Library Manager yet. Install it by downloading the repository ZIP or cloning it into the Arduino libraries directory.

```txt
Arduino/libraries/Worker
```

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
        Serial.println(initResult.message.c_str());
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

No cleanup call is required after `once()` or `every()`. Worker releases callback captures, task stacks, task TCBs, and active job records automatically.

## Cleanup model

Worker creates one long-lived internal cleanup task during `init()`.

When a job callback finishes, the job:

1. releases its stored callback;
2. queues its handle and immutable allocation type;
3. suspends itself.

The cleanup task then deletes the job externally with the correct FreeRTOS API. Worker emits the completion event and removes the active record only after deletion returns.

This avoids the ESP-IDF temporary-task path used when a capability-created task calls `vTaskDeleteWithCaps()` on itself.

`waitFor()` and `stopAndWait()` are optional synchronization APIs. They wait for physical task cleanup; they do not perform cleanup.

## Important notes

> [!IMPORTANT]
> Worker cancellation is cooperative. `stop()` requests that a job stops and wakes it if it is sleeping, but it does not interrupt a running callback.

- A callback that blocks forever prevents timed `stopAndWait()` and `end()` calls from completing.
- The destructor waits without a timeout so tasks cannot outlive Worker internals.
- `every(intervalMs, callback)` delays after each callback.
- `WorkerStackType::Auto` prefers PSRAM task stacks when supported and falls back to internal RAM.
- Stack sizes are FreeRTOS byte sizes on ESP32 and must be at least 1024 bytes.
- `maxConcurrentJobs` bounds active jobs and guarantees cleanup queue capacity.
- `clearFinished()` is retained only as a deprecated compatibility no-op.
- Completion synchronization uses a small bounded token window and never retains callbacks or full completed records.
- Worker APIs use result objects for normal failures. Catastrophic STL allocation failure is not recoverable on platforms where the standard library aborts.

## Examples

| Example | Description |
| --- | --- |
| `Basic` | Minimal initialization, one-off job, recurring job, wait, and cooperative stop. |
| `JobConfig` | Stack size, priority, core affinity, internal stack, and PSRAM stack request. |
| `Events` | Event callback and error event handling. |
| `SleepAndWait` | Context sleep, external sleep, wait, and timeout behavior. |
| `Diagnostics` | Current job and cleanup-task diagnostics. |
| `BindableCallbacks` | `std::bind` with private class methods. |
| `TaskCleanupSentinel` | Fire-and-forget capture, heap, PSRAM, and task-count cleanup checks. |

Start with:

```txt
examples/Basic
```

## Documentation

| Document | Description |
| --- | --- |
| [`docs/getting-started.md`](docs/getting-started.md) | Setup and first jobs. |
| [`docs/configuration.md`](docs/configuration.md) | Job defaults and cleanup infrastructure. |
| [`docs/api.md`](docs/api.md) | Public API and cleanup semantics. |
| [`docs/examples.md`](docs/examples.md) | Example descriptions. |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | Common lifecycle and configuration issues. |

## API overview

```cpp
Worker worker;
worker.init();
worker.onEvent([](WorkerEvent event) {});

WorkerJobResult once = worker.once([](WorkerJobContext &ctx) {});
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
| Filesystem | none |
| PSRAM | Optional task stacks through ESP-IDF capability APIs |
| Dependencies | none |
| Exceptions | Not used |
| Status | Early-stage `0.1.0` |

## License

MIT — see [`LICENSE.md`](LICENSE.md).

## ZekStack

Part of the ZekStack ESP32 library stack.
