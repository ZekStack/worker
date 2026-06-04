#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class WorkerMutex {
  public:
	WorkerMutex() {
		_handle = xSemaphoreCreateRecursiveMutex();
	}

	~WorkerMutex() {
		if (_handle != nullptr) {
			vSemaphoreDelete(_handle);
		}
	}

	WorkerMutex(const WorkerMutex &) = delete;
	WorkerMutex &operator=(const WorkerMutex &) = delete;

	bool lock(TickType_t timeout = portMAX_DELAY) {
		return _handle != nullptr && xSemaphoreTakeRecursive(_handle, timeout) == pdTRUE;
	}

	void unlock() {
		if (_handle != nullptr) {
			xSemaphoreGiveRecursive(_handle);
		}
	}

  private:
	SemaphoreHandle_t _handle = nullptr;
};

class WorkerLock {
  public:
	explicit WorkerLock(WorkerMutex &mutex) : _mutex(mutex), _locked(mutex.lock()) {
	}

	~WorkerLock() {
		if (_locked) {
			_mutex.unlock();
		}
	}

	WorkerLock(const WorkerLock &) = delete;
	WorkerLock &operator=(const WorkerLock &) = delete;

	explicit operator bool() const {
		return _locked;
	}

  private:
	WorkerMutex &_mutex;
	bool _locked = false;
};
