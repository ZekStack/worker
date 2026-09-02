# Examples

Worker examples are compiled in CI with both PIOArduino and Arduino CLI across ESP32, ESP32-S3, ESP32-C3, and ESP32-P4. CI installs the pinned Strata `v0.1.1` dependency before building them.

| Example | What it demonstrates |
| --- | --- |
| `Basic` | Initialization, one-off/recurring jobs, wait, and cooperative stop. |
| `JobConfig` | Worker `MemoryPolicy`, inherited task placement, and per-job `Internal` / `RequireExternal` overrides. |
| `Events` | Synchronous Worker events and normal error reporting. |
| `SleepAndWait` | Cooperative sleep, external sleep requests, wait, and timeout behavior. |
| `Diagnostics` | Requested Strata placement versus observed stack/cleanup memory regions. |
| `BindableCallbacks` | `std::bind` and private method callbacks. |
| `TaskCleanupSentinel` | Repeated fire-and-forget cleanup while Worker general allocations and task stacks use `PreferExternal`. |

Start with `Basic`, then use `JobConfig` and `Diagnostics` when integrating Worker memory policy into an application.
