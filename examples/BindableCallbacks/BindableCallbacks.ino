#include <Arduino.h>
#include <Worker.h>
#include <functional>

class JobOwner {
  public:
	explicit JobOwner(Worker &worker) : _worker(worker) {
	}

	void begin() {
		_worker.every(
		    1000,
		    std::bind(&JobOwner::run, this, std::placeholders::_1)
		);
	}

  private:
	void run(WorkerJobContext &ctx) {
		Serial.printf("bound callback run=%u\n", static_cast<unsigned>(ctx.runCount()));
		if (ctx.runCount() >= 5) {
			ctx.stop();
		}
	}

	Worker &_worker;
};

Worker worker;
JobOwner owner(worker);

void setup() {
	Serial.begin(115200);

	WorkerResult initResult = worker.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	owner.begin();
}

void loop() {
	delay(1000);
}
