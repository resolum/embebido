#ifndef DEVICE_H
#define DEVICE_H

/**
 * @file Device.h
 * @brief Device = event/command handler plus a two-task scheduler layer (FreeRTOS).
 *
 *   hardware worker: every tick sends MEASURE_DATA_REQUESTED to each registered sensor.
 *                    The sensor's reply reaches Device::on(), which may enqueue a TelemetryPackage.
 *   network worker : takes packages off the queue and calls processQueuedTelemetryData(),
 *                    then deletes them. Slow I/O (Serial, later the edge link) lives here.
 *
 * Ownership: after a successful enqueueTelemetryPayload() the Device owns the package.
 */

#include <Arduino.h>

#include "CommandHandler.h"
#include "EventHandler.h"
#include "Sensor.h"
#include "TelemetryPackage.h"

class Device : public EventHandler, public CommandHandler {
public:
    explicit Device(unsigned long schedulerTickInMilliseconds);
    ~Device() override;

    void on(Event event) override = 0;
    void handle(Command command) override = 0;

protected:
    static constexpr size_t MAX_SCHEDULED_SENSORS = 4;

    /** @brief Asks `sensor` for `eventIdentifier` on every tick. False when the table is full. */
    bool appendSensorToScheduler(Sensor* sensor, int eventIdentifier);

    /** @brief Creates the telemetry queue and starts both workers. Call once, after registering sensors. */
    bool initializeAsynchronousEngine(size_t queueDepth);

    /** @brief Hands `*package` to the network worker. True = queued (ownership moved), false = full/not started. */
    bool enqueueTelemetryPayload(TelemetryPackage** package);

    virtual void processQueuedTelemetryData(const TelemetryPackage* telemetryPackage) const = 0;

private:
    struct ScheduledSensor {
        Sensor* sensor = nullptr;
        int eventIdentifier = 0;
    };

    static void hardwareWorkerEntry(void* device);
    static void networkWorkerEntry(void* device);

    const unsigned long tickInMilliseconds;
    ScheduledSensor scheduledSensors[MAX_SCHEDULED_SENSORS];
    size_t scheduledSensorCount = 0;
    QueueHandle_t telemetryQueue = nullptr;
    TaskHandle_t hardwareWorker = nullptr;
    TaskHandle_t networkWorker = nullptr;
};

#endif // DEVICE_H
