#include <Arduino.h>
#include <Worker.h>

Worker worker;

void setup() {
	Serial.begin(115200);

	WorkerResult initResult = worker.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	WorkerJobResult sleeper = worker.every(1000, [](WorkerJobContext &ctx) {
		Serial.printf("sleeping job run=%u\n", static_cast<unsigned>(ctx.runCount()));
		if (ctx.runCount() == 2) {
			ctx.sleep(2000);
		}
		if (ctx.runCount() >= 4) {
			ctx.stop();
		}
	});

	if (sleeper) {
		worker.sleep(sleeper.jobId, 1500);
		WorkerResult waitResult = worker.waitFor(sleeper.jobId, 10000);
		Serial.println(waitResult.message.c_str());
	}

	WorkerJobResult blocking = worker.every(1000, [](WorkerJobContext &) {
		delay(5000);
	});

	if (blocking) {
		WorkerResult stopResult = worker.stopAndWait(blocking.jobId, 100);
		Serial.println(stopResult.message.c_str());
	}
}

void loop() {
	delay(1000);
}
