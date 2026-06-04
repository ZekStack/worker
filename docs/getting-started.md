# Getting Started

Include `Worker.h`, create a `Worker` instance, and call `init()` before creating jobs.

```cpp
#include <Arduino.h>
#include <Worker.h>

Worker worker;

void setup() {
	Serial.begin(115200);

	WorkerResult result = worker.init();
	if (!result) {
		Serial.println(result.message.c_str());
		return;
	}
}
```

Create a one-off job with `once()`.

```cpp
worker.once([](WorkerJobContext &ctx) {
	Serial.printf("job id=%u\n", static_cast<unsigned>(ctx.id()));
});
```

Create a recurring job with `every()`.

```cpp
worker.every(1000, [](WorkerJobContext &ctx) {
	Serial.println("runs every second");
	if (ctx.runCount() >= 5) {
		ctx.stop();
	}
});
```

`every()` delays internally after the callback returns. Do not add a delay only to protect the system from spinning.
