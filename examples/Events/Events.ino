#include <Arduino.h>
#include <Worker.h>

Worker worker;

void setup() {
	Serial.begin(115200);

	worker.onEvent([](WorkerEvent event) {
		Serial.printf("Worker event occurred: %s\n", event.message);
		if (event.isError()) {
			Serial.printf("error status=%u\n", static_cast<unsigned>(event.status));
		}
	});

	WorkerResult initResult = worker.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	worker.once([](WorkerJobContext &ctx) {
		Serial.printf("job id=%u\n", static_cast<unsigned>(ctx.id()));
	});

	WorkerJobConfig invalidConfig;
	invalidConfig.stackSize = 7;
	worker.once(invalidConfig, [](WorkerJobContext &ctx) {
		Serial.printf("this should not run id=%u\n", static_cast<unsigned>(ctx.id()));
	});
}

void loop() {
	delay(1000);
}
