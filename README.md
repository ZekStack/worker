# Worker

Worker is a FreeRTOS task and cooperative job execution library for ESP32.

Worker helps you run one-off and recurring background work in Arduino ESP32 projects with explicit task configuration, cooperative stop/sleep controls, event reporting, and diagnostics. It is designed for products that need predictable task behavior without spreading raw FreeRTOS task management across the app.

[![CI](https://github.com/ZekStack/worker/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/worker/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/worker?sort=semver)](https://github.com/ZekStack/worker/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Why use Worker?

* **Task-per-job execution** - each `once()` and `every()` job owns a FreeRTOS task.
* **Safe recurring jobs** - `every()` applies the interval delay internally after each callback.
* **ESP32 task control** - configure byte stack size, priority, core affinity, and stack memory preference.
* **Cooperative lifecycle** - jobs can stop or sleep through `WorkerJobContext`.
* **Production-minded** - result-based errors, event callbacks, diagnostics, thread-safe internals, and no intentional exceptions.

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

Worker is not published to Arduino Library Manager yet.

Install it by downloading the repository ZIP or cloning it into your Arduino libraries folder.

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

## Important notes

> [!IMPORTANT]
> Worker cancellation is cooperative. `stop()` requests that a job stops and wakes it if it is sleeping, but it does not force-delete a running callback.

* A callback that blocks forever will prevent `stopAndWait()` and timed `end()` calls from completing before their timeout. The destructor waits without a timeout so tasks cannot outlive Worker internals.
* `every(intervalMs, callback)` delays internally after each callback.
* Completed jobs are retained until `waitFor()` can consume them after task cleanup, `clearFinished()` is called, or Worker ends.
* Stack sizes are FreeRTOS byte sizes on ESP32 and must be at least 1024 bytes.
* Custom task names are copied into a fixed internal buffer and may be truncated.
* `WorkerStackType::Auto` prefers PSRAM task stacks when supported and falls back to internal RAM.
* `WorkerEvent::type` clearly identifies `Info`, `Warning`, or `Error` events.
* Worker APIs use result objects for normal failures. Catastrophic STL allocation failure while constructing result messages, callbacks, or internal containers is not recoverable by Worker on platforms where the standard library throws or aborts.

## Examples

| Example | Description |
| --- | --- |
| `Basic` | Minimal init, one-off job, recurring job, wait, and cooperative stop. |
| `JobConfig` | Stack size, priority, core affinity, internal stack, and PSRAM stack request. |
| `Events` | Event callback and error event handling. |
| `SleepAndWait` | Context sleep, external sleep, wait, and timeout behavior. |
| `Diagnostics` | Aggregate diagnostics and active per-job diagnostics. |
| `BindableCallbacks` | `std::bind` with private class methods. |
| `TaskCleanupSentinel` | Runtime sentinel for verifying task-entry C++ cleanup before `waitFor()` returns. |

Start with:

```txt
examples/Basic
```

## Documentation

Detailed documentation is available in the `docs/` folder.

| Document | Description |
| --- | --- |
| [`docs/getting-started.md`](docs/getting-started.md) | Step-by-step setup and first job flow. |
| [`docs/configuration.md`](docs/configuration.md) | Config options and stack behavior. |
| [`docs/api.md`](docs/api.md) | Public classes, result types, events, and diagnostics. |
| [`docs/examples.md`](docs/examples.md) | Explanation of all included examples. |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | Common issues and solutions. |

## API overview

```cpp
Worker worker;
worker.init();
worker.onEvent([](WorkerEvent event) {});

WorkerJobResult once = worker.once([](WorkerJobContext &ctx) {});
WorkerJobResult loop = worker.every(1000, [](WorkerJobContext &ctx) {});

worker.sleep(loop.jobId, 5000);

WorkerDiag diag = worker.getDiagnostics();
WorkerJobDiag jobDiag;
worker.getJobDiagnostics(loop.jobId, jobDiag);

worker.stopAndWait(loop.jobId, 2000);
worker.clearFinished();
```

For the full API, see [`docs/api.md`](docs/api.md).

## Compatibility

| Item | Support |
| --- | --- |
| Framework | Arduino ESP32 |
| Platform | `espressif32` |
| Language | C++20 |
| Filesystem | none |
| PSRAM | Optional for task stacks when ESP-IDF support is available |
| Dependencies | none |
| Exceptions | Not used |
| Status | Early-stage `0.1.0` |

## Configuration

```cpp
WorkerConfig config;
config.defaultStackSize = 4096;
config.defaultPriority = 1;
config.defaultCoreId = tskNO_AFFINITY;
config.defaultStackType = WorkerStackType::Auto;

WorkerResult result = worker.init(config);
```

For all options, see [`docs/configuration.md`](docs/configuration.md).

## Error handling

Worker reports operation status through `WorkerResult` and `WorkerJobResult`.

```cpp
WorkerJobResult result = worker.every(1000, [](WorkerJobContext &ctx) {});

if (!result) {
	Serial.println(result.message.c_str());
	return;
}
```

For result fields and status codes, see [`docs/api.md`](docs/api.md).

## License

MIT - see [`LICENSE.md`](LICENSE.md).

## ZekStack

Part of the ZekStack ESP32 library stack.
