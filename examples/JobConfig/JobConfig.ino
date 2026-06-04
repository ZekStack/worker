#include <Arduino.h>
#include <Worker.h>

Worker worker;

void setup() {
	Serial.begin(115200);

	WorkerConfig config;
	config.defaultStackSize = 4096;
	config.defaultPriority = 1;
	config.defaultCoreId = tskNO_AFFINITY;
	config.defaultStackType = WorkerStackType::Auto;

	WorkerResult initResult = worker.init(config);
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	WorkerJobConfig importantJob;
	importantJob.name = "important";
	importantJob.stackSize = 8192;
	importantJob.priority = 2;
	importantJob.coreId = tskNO_AFFINITY;
	importantJob.stackType = WorkerStackType::Internal;

	worker.once(importantJob, [](WorkerJobContext &ctx) {
		Serial.printf("configured job id=%u\n", static_cast<unsigned>(ctx.id()));
	});

	WorkerJobConfig psramJob;
	psramJob.name = "psram";
	psramJob.stackSize = 8192;
	psramJob.stackType = WorkerStackType::Psram;

	WorkerJobResult psramResult = worker.once(psramJob, [](WorkerJobContext &ctx) {
		Serial.printf("PSRAM stack job id=%u\n", static_cast<unsigned>(ctx.id()));
	});

	if (!psramResult) {
		Serial.println(psramResult.message.c_str());
	}
}

void loop() {
	delay(1000);
}
