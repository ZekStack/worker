#include <Arduino.h>
#include <Worker.h>

#include <assert.h>
#include <atomic>
#include <memory>

extern "C" {
#include "esp_heap_caps.h"
#include "freertos/task.h"
}

namespace {
constexpr size_t kWarmupJobs = 32;
constexpr size_t kStressJobs = 256;
constexpr uint32_t kIdleTimeoutMs = 5000;
constexpr size_t kHeapToleranceBytes = 512;

struct Probe {
	std::atomic<size_t> *destroyed = nullptr;

	~Probe() {
		if (destroyed != nullptr) {
			destroyed->fetch_add(1);
		}
	}
};

Worker worker;

bool waitUntilIdle(uint32_t timeoutMs) {
	const uint32_t startedAt = millis();
	while (static_cast<uint32_t>(millis() - startedAt) < timeoutMs) {
		const WorkerDiag diag = worker.getDiagnostics();
		if (diag.activeJobCount == 0 && diag.cleanupQueuedCount == 0 &&
		    diag.cleanupQueueDepth == 0) {
			return true;
		}
		delay(1);
	}
	return false;
}

void runFireAndForgetBatch(size_t jobCount, std::atomic<size_t> &destroyed) {
	for (size_t index = 0; index < jobCount; ++index) {
		auto probe = std::make_shared<Probe>();
		probe->destroyed = &destroyed;

		while (true) {
			WorkerJobResult result = worker.once([probe](WorkerJobContext &) {});
			if (result) {
				break;
			}
			assert(result.status == WorkerStatus::Busy);
			delay(1);
		}
		probe.reset();
	}
	assert(waitUntilIdle(kIdleTimeoutMs));
}
} // namespace

void setup() {
	Serial.begin(115200);

	WorkerResult initResult = worker.init();
	assert(initResult);

	std::atomic<size_t> warmupDestroyed{0};
	runFireAndForgetBatch(kWarmupJobs, warmupDestroyed);
	assert(warmupDestroyed.load() == kWarmupJobs);

	const size_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	const size_t psramBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	const UBaseType_t tasksBefore = uxTaskGetNumberOfTasks();

	std::atomic<size_t> destroyed{0};
	runFireAndForgetBatch(kStressJobs, destroyed);
	assert(destroyed.load() == kStressJobs);

	const WorkerDiag diag = worker.getDiagnostics();
	const size_t internalAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	const size_t psramAfter = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	const UBaseType_t tasksAfter = uxTaskGetNumberOfTasks();

	assert(diag.activeJobCount == 0);
	assert(diag.cleanupQueuedCount == 0);
	assert(diag.cleanupQueueDepth == 0);
	assert(tasksAfter == tasksBefore);
	assert(internalAfter + kHeapToleranceBytes >= internalBefore);
	assert(psramAfter + kHeapToleranceBytes >= psramBefore);

	Serial.printf(
	    "cleanup sentinel passed: internal=%d psram=%d tasks=%u\n",
	    static_cast<int>(internalAfter) - static_cast<int>(internalBefore),
	    static_cast<int>(psramAfter) - static_cast<int>(psramBefore),
	    static_cast<unsigned>(tasksAfter)
	);
}

void loop() {
	delay(1000);
}
