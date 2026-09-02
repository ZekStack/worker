#include <Arduino.h>
#include <Worker.h>

Worker worker;

void setup() {
	Serial.begin(115200);

	WorkerConfig config;
	config.memory.allocation = Strata::Placement::Default;
	config.memory.taskStack = Strata::Placement::PreferExternal;
	config.defaultStackSize = 4096;
	config.defaultPriority = 1;
	config.defaultCoreId = tskNO_AFFINITY;

	WorkerResult initResult = worker.init(config);
	if (!initResult) {
		Serial.println(initResult.message);
		return;
	}

	WorkerJobConfig importantJob;
	importantJob.name = "important";
	importantJob.stackSize = 8192;
	importantJob.priority = 2;
	importantJob.coreId = tskNO_AFFINITY;
	importantJob.stackPlacement = Strata::Placement::Internal;

	worker.once(importantJob, [](WorkerJobContext &ctx) {
		Serial.printf("configured job id=%u\n", static_cast<unsigned>(ctx.id()));
	});

	WorkerJobConfig externalJob;
	externalJob.name = "external";
	externalJob.stackSize = 8192;
	externalJob.stackPlacement = Strata::Placement::RequireExternal;

	WorkerJobResult externalResult = worker.once(externalJob, [](WorkerJobContext &ctx) {
		Serial.printf("external stack job id=%u\n", static_cast<unsigned>(ctx.id()));
	});

	if (!externalResult) {
		Serial.println(externalResult.message);
	}
}

void loop() {
	delay(1000);
}
