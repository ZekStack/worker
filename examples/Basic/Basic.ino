#include <Arduino.h>
#include <Worker.h>

Worker worker;
WorkerJobId recurringJobId = 0;

void setup() {
	Serial.begin(115200);

	WorkerResult initResult = worker.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	WorkerJobResult onceResult = worker.once([](WorkerJobContext &ctx) {
		Serial.printf("one-off job id=%u\n", static_cast<unsigned>(ctx.id()));
	});

	if (onceResult) {
		worker.waitFor(onceResult.jobId, 2000);
	}

	WorkerJobResult everyResult = worker.every(1000, [](WorkerJobContext &ctx) {
		Serial.printf("recurring run=%u\n", static_cast<unsigned>(ctx.runCount()));
		if (ctx.runCount() >= 5) {
			ctx.stop();
		}
	});

	if (everyResult) {
		recurringJobId = everyResult.jobId;
	}
}

void loop() {
	delay(1000);
}
