#include <Arduino.h>
#include <Worker.h>

Worker worker;
WorkerJobId jobId = 0;

void printDiagnostics() {
	WorkerDiag diag = worker.getDiagnostics();
	Serial.printf(
	    "active=%u running=%u sleeping=%u stopping=%u cleanup=%u queue=%u cleanupTask=%s\n",
	    static_cast<unsigned>(diag.activeJobCount),
	    static_cast<unsigned>(diag.runningJobCount),
	    static_cast<unsigned>(diag.sleepingJobCount),
	    static_cast<unsigned>(diag.stoppingJobCount),
	    static_cast<unsigned>(diag.cleanupQueuedCount),
	    static_cast<unsigned>(diag.cleanupQueueDepth),
	    diag.cleanupTaskRunning ? "running" : "stopped"
	);

	WorkerJobDiag jobDiag;
	WorkerResult result = worker.getJobDiagnostics(jobId, jobDiag);
	if (result) {
		Serial.printf(
		    "job=%u state=%s name=%s runs=%u stack=%u\n",
		    static_cast<unsigned>(jobDiag.jobId),
		    worker.jobStateToString(jobDiag.state),
		    jobDiag.name,
		    static_cast<unsigned>(jobDiag.runCount),
		    static_cast<unsigned>(jobDiag.stackSize)
		);
	} else {
		Serial.printf("job diagnostics unavailable: %s\n", result.message.c_str());
	}
}

void setup() {
	Serial.begin(115200);

	WorkerResult initResult = worker.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	WorkerJobConfig config;
	config.name = "diag-job";
	config.stackSize = 4096;

	WorkerJobResult result = worker.every(500, config, [](WorkerJobContext &ctx) {
		if (ctx.runCount() >= 3) {
			ctx.stop();
		}
	});

	if (result) {
		jobId = result.jobId;
		printDiagnostics();
		worker.waitFor(jobId, 5000);
		printDiagnostics();
	}
}

void loop() {
	delay(1000);
}
