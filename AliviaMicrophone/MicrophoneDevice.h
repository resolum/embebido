#ifndef MICROPHONE_DEVICE_H
#define MICROPHONE_DEVICE_H

/**
 * @file MicrophoneDevice.h
 * @brief ESP32 #1 of Alivia Voice: the INMP441 microphone as a ModestIoT Device.
 *
 * Flow inside the framework:
 *   hardware timer -> HardwareWorker -> microphone.on(MEASURE_DATA_REQUESTED_EVENT)
 *     -> SOUND_STARTED / SOUND_ENDED -> VoiceActivityTelemetry
 *     -> DATA_READ                   -> AudioLevelTelemetry
 *   telemetry queue -> NetworkWorker -> processQueuedTelemetryData() -> Serial and, when the link is up, the edge
 *
 * The audio itself never goes through the telemetry queue: the capture task in
 * Inmp441Microphone handles it so the 16 kHz stream is never late.
 */

#include "Device.h"
#include "EdgeLink.h"
#include "Inmp441Microphone.h"

class MicrophoneDevice : public Device {
public:
    MicrophoneDevice(const char* deviceIdentifier, Inmp441Microphone& microphoneSensor,
                     unsigned long schedulerTickInMilliseconds);

    /** @brief Registers the microphone in the scheduler and starts the framework tasks. */
    void begin();

    void on(Event event) override;
    void handle(Command command) override;

    /** @brief Also sends every telemetry JSON to the edge over the WebSocket while the link is ready. */
    void attachLink(EdgeLink* edgeLink);

    void setJsonOutputEnabled(bool enabled);
    [[nodiscard]] bool isJsonOutputEnabled() const;
    /** @brief Hides the periodic level lines (used while the live VU meter is on). */
    void setReportsVisible(bool visible);

protected:
    void processQueuedTelemetryData(const TelemetryPackage* telemetryPackage) const override;

private:
    const char* deviceId;
    Inmp441Microphone& microphone;
    EdgeLink* link = nullptr;
    volatile bool jsonOutputEnabled = false;
    volatile bool reportsVisible = true;
};

#endif // MICROPHONE_DEVICE_H
