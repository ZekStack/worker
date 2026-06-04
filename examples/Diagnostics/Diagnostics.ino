#include <Arduino.h>
#include <Worker.h>

Worker worker;
WorkerJobId jobId = 0;

void printDiagnostics() {
	WorkerDiag diag = worker.getDiagnostics();
	Serial.printf("jobs=%u running=%u sleeping=%u finished=%u stopped=%u failed=%u\n",
	    static_cast<unsigned>(diag.totalJobCount),
	    static_cast<unsigned>(diag.runningJobCount),
	    static_cast<unsigned>(diag.sleepingJobCount),
	    static_cast<unsigned>(diag.finishedJobCount),
	    static_cast<unsigned>(diag.stoppedJobCount),
	    static_cast<unsigned>(diag.failedJobCount));

	WorkerJobDiag jobDiag;
	WorkerResult result = worker.getJobDiagnostics(jobId, jobDiag);
	if (result) {
		Serial.printf("job=%u name=%s runs=%u stack=%u\n",
		    static_cast<unsigned>(jobDiag.jobId),
		    jobDiag.name,
		    static_cast<unsigned>(jobDiag.runCount),
		    static_cast<unsigned>(jobDiag.stackSize));
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
		worker.waitFor(jobId, 5000);
		printDiagnostics();
	}
}

void loop() {
	delay(1000);
}
