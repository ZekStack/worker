#include <Arduino.h>
#include <Worker.h>

#include <assert.h>
#include <atomic>
#include <memory>

struct Probe {
	std::atomic<int> *destroyed = nullptr;

	~Probe() {
		if (destroyed != nullptr) {
			destroyed->fetch_add(1);
		}
	}
};

Worker worker;

void runOneShotCleanupSentinel() {
	std::atomic<int> destroyed{0};
	auto probe = std::make_shared<Probe>();
	probe->destroyed = &destroyed;

	WorkerJobResult result = worker.once([probe](WorkerJobContext &) {});
	assert(result);

	probe.reset();
	assert(destroyed.load() == 0);

	WorkerResult waitResult = worker.waitFor(result.jobId, 2000);
	assert(waitResult);
	assert(destroyed.load() == 1);

	worker.clearFinished();
}

void runRecurringCleanupSentinel() {
	std::atomic<int> destroyed{0};
	auto probe = std::make_shared<Probe>();
	probe->destroyed = &destroyed;

	WorkerJobResult result = worker.every(10, [probe](WorkerJobContext &ctx) {
		ctx.stop();
	});
	assert(result);

	probe.reset();
	assert(destroyed.load() == 0);

	WorkerResult waitResult = worker.waitFor(result.jobId, 2000);
	assert(waitResult);
	assert(destroyed.load() == 1);

	worker.clearFinished();
}

void setup() {
	Serial.begin(115200);

	WorkerResult initResult = worker.init();
	assert(initResult);

	runOneShotCleanupSentinel();
	runRecurringCleanupSentinel();

	Serial.println("task cleanup sentinel passed");
}

void loop() {
	delay(1000);
}
