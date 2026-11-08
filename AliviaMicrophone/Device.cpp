#include "Device.h"

Device::Device(unsigned long schedulerTickInMilliseconds) : tickInMilliseconds(schedulerTickInMilliseconds) {}

Device::~Device() {
    if (hardwareWorker != nullptr) vTaskDelete(hardwareWorker);
    if (networkWorker != nullptr) vTaskDelete(networkWorker);
    if (telemetryQueue != nullptr) {
        TelemetryPackage* pending = nullptr;
        while (xQueueReceive(telemetryQueue, &pending, 0) == pdTRUE) delete pending;
        vQueueDelete(telemetryQueue);
    }
}

bool Device::appendSensorToScheduler(Sensor* sensor, int eventIdentifier) {
    if (sensor == nullptr || scheduledSensorCount >= MAX_SCHEDULED_SENSORS) return false;
    scheduledSensors[scheduledSensorCount++] = {sensor, eventIdentifier};
    return true;
}

bool Device::initializeAsynchronousEngine(size_t queueDepth) {
    if (telemetryQueue != nullptr) return true;  // already running
    telemetryQueue = xQueueCreate(queueDepth, sizeof(TelemetryPackage*));
    if (telemetryQueue == nullptr) return false;

    const bool started =
        xTaskCreate(networkWorkerEntry, "network", 6144, this, 1, &networkWorker) == pdPASS &&
        xTaskCreate(hardwareWorkerEntry, "hardware", 4096, this, 2, &hardwareWorker) == pdPASS;
    return started;
}

bool Device::enqueueTelemetryPayload(TelemetryPackage** package) {
    if (telemetryQueue == nullptr || package == nullptr || *package == nullptr) return false;
    return xQueueSend(telemetryQueue, package, 0) == pdTRUE;
}

void Device::hardwareWorkerEntry(void* device) {
    auto* self = static_cast<Device*>(device);
    const TickType_t period = pdMS_TO_TICKS(self->tickInMilliseconds);
    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&lastWake, period > 0 ? period : 1);
        for (size_t i = 0; i < self->scheduledSensorCount; ++i) {
            const ScheduledSensor& entry = self->scheduledSensors[i];
            entry.sensor->on(Event(entry.eventIdentifier));
        }
    }
}

void Device::networkWorkerEntry(void* device) {
    auto* self = static_cast<Device*>(device);
    for (;;) {
        TelemetryPackage* package = nullptr;
        if (xQueueReceive(self->telemetryQueue, &package, portMAX_DELAY) == pdTRUE && package != nullptr) {
            self->processQueuedTelemetryData(package);
            delete package;
        }
    }
}
