#include "MicrophoneDevice.h"

#include "AudioTelemetry.h"
#include "MicrophoneConfig.h"
#include "SerialConsole.h"

namespace {

/** Draws a 20-character bar for a level between -90 dBFS and 0 dBFS. */
void buildLevelBar(float levelInDbfs, char* bar, size_t barSize) {
    constexpr int width = 20;
    int filled = static_cast<int>((levelInDbfs + 90.0f) / 90.0f * width + 0.5f);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;
    size_t i = 0;
    for (; i < static_cast<size_t>(width) && i + 1 < barSize; ++i) {
        bar[i] = static_cast<int>(i) < filled ? '#' : '.';
    }
    bar[i] = '\0';
}

const char* slotName(int slot) {
    return slot == 0 ? "L" : slot == 1 ? "R" : "?";
}

}  // namespace

MicrophoneDevice::MicrophoneDevice(const char* deviceIdentifier, Inmp441Microphone& microphoneSensor,
                                   unsigned long schedulerTickInMilliseconds)
    : Device(schedulerTickInMilliseconds),
      deviceId(deviceIdentifier),
      microphone(microphoneSensor) {}

void MicrophoneDevice::begin() {
    microphone.setHandler(this);
    appendSensorToScheduler(&microphone, Sensor::MEASURE_DATA_REQUESTED_EVENT_IDENTIFIER);
    initializeAsynchronousEngine(16);
}

void MicrophoneDevice::on(Event event) {
    TelemetryPackage* package = nullptr;

    if (event.identifier == Inmp441Microphone::SOUND_STARTED_EVENT_IDENTIFIER) {
        package = new VoiceActivityTelemetry(deviceId, true, microphone.getInstantLevelInDbfs());
    } else if (event.identifier == Inmp441Microphone::SOUND_ENDED_EVENT_IDENTIFIER) {
        package = new VoiceActivityTelemetry(deviceId, false, microphone.getInstantLevelInDbfs());
    } else if (event.identifier == Sensor::DATA_READ_EVENT_IDENTIFIER) {
        package = new AudioLevelTelemetry(deviceId, microphone.getLastReport());
    }

    if (package != nullptr && !enqueueTelemetryPayload(&package)) {
        delete package;  // queue full: the NetworkWorker is behind, skip this report
    }
}

void MicrophoneDevice::handle(Command command) {
    (void)command;  // no actuators in this device
}

void MicrophoneDevice::processQueuedTelemetryData(const TelemetryPackage* telemetryPackage) const {
    JsonDocument document;
    telemetryPackage->serialize(document);
    const char* type = document["type"] | "";

    if (strcmp(type, "voice_activity") == 0) {
        const bool active = document["active"] | false;
        const float level = document["levelDbfs"] | -120.0f;
        SerialConsole::printf(active ? "[Mic] >>> Sonido detectado (%.1f dBFS)\n"
                                     : "[Mic] <<< Silencio (%.1f dBFS)\n", level);
    } else if (strcmp(type, "audio_level") == 0 && reportsVisible) {
        const auto* levelPackage = static_cast<const AudioLevelTelemetry*>(telemetryPackage);
        const AudioLevelReport& r = levelPackage->report;
        char bar[24];
        buildLevelBar(r.rmsInDbfs, bar, sizeof(bar));
        SerialConsole::printf(
            "[Mic] %s %6.1f dBFS | pico %6.1f dBFS (%5d) | ruido %6.1f | voz %-2s | clip %lu | "
            "%5.0f Hz | canal %s | gan %.0f dB | perdidas %lu | %s\n",
            bar, r.rmsInDbfs, r.peakInDbfs, r.peakAmplitude, r.noiseFloorInDbfs,
            r.voiceActive ? "SI" : "no", static_cast<unsigned long>(r.clippedSamples),
            r.measuredSampleRateInHertz, slotName(r.activeSlot), r.gainInDecibels,
            static_cast<unsigned long>(r.droppedFrames), Inmp441Microphone::describeStatus(r.status));
        if (r.status != MicrophoneSignalStatus::OK) {
            SerialConsole::printf("      -> %s\n", Inmp441Microphone::describeStatusHint(r.status));
        }
    }

    if (jsonOutputEnabled && (reportsVisible || strcmp(type, "audio_level") != 0)) {
        String json;
        serializeJson(document, json);
        SerialConsole::printf("%s\n", json.c_str());
    }

    if (link != nullptr) {
        const bool isLevel = strcmp(type, "audio_level") == 0;
        if (isLevel) {
            link->addMicQueueDrops(static_cast<const AudioLevelTelemetry*>(telemetryPackage)->report.droppedFrames);
        }
        if (link->isReady() && (!isLevel || SEND_AUDIO_LEVEL_TELEMETRY)) {
            String json;
            serializeJson(document, json);
            link->sendText(json.c_str());
        }
    }
}

void MicrophoneDevice::attachLink(EdgeLink* edgeLink) { link = edgeLink; }

void MicrophoneDevice::setJsonOutputEnabled(bool enabled) { jsonOutputEnabled = enabled; }

bool MicrophoneDevice::isJsonOutputEnabled() const { return jsonOutputEnabled; }

void MicrophoneDevice::setReportsVisible(bool visible) { reportsVisible = visible; }
