# Getting started

Worker `v0.2.0` requires Strata `v0.1.1` and C++20.

PlatformIO resolves the pinned Strata dependency from Worker's `library.json`. For Arduino IDE/manual installs, place both repositories in the Arduino libraries directory.

```cpp
#include <Arduino.h>
#include <Worker.h>

Worker worker;

void setup() {
    Serial.begin(115200);

    WorkerConfig config;
    config.memory.allocation = Strata::Placement::Default;
    config.memory.taskStack = Strata::Placement::PreferExternal;

    WorkerResult initResult = worker.init(config);
    if (!initResult) {
        Serial.println(initResult.message);
        return;
    }

    worker.once([](WorkerJobContext &ctx) {
        Serial.printf("job=%u\n", static_cast<unsigned>(ctx.id()));
    });
}

void loop() {
    delay(1000);
}
```

The default Worker task-stack policy is `PreferExternal`, which preserves the old automatic external-stack preference while allowing internal fallback. General allocations use `Placement::Default` unless configured otherwise.

For a task that must stay internal:

```cpp
WorkerJobConfig job;
job.stackPlacement = Strata::Placement::Internal;
worker.once(job, [](WorkerJobContext &) {});
```

For an application that wants all movable Worker storage and normal Worker task stacks to prefer external memory:

```cpp
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;
```

Worker cleanup is automatic. `waitFor()` and `stopAndWait()` are synchronization tools only; callers never free task stacks, TCBs, queues, mutexes, or job records themselves.
