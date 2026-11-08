#ifndef AUDIO_TELEMETRY_H
#define AUDIO_TELEMETRY_H

/**
 * @file AudioTelemetry.h
 * @brief Telemetry packages produced by the microphone device.
 *
 * Today they are printed on Serial. When the edge link exists, the same JSON
 * goes to the edge from MicrophoneDevice::processQueuedTelemetryData().
 */

#include "TelemetryPackage.h"
#include "Inmp441Microphone.h"

/** @brief Periodic level report (one per report interval). */
class AudioLevelTelemetry : public TelemetryPackage {
public:
    AudioLevelTelemetry(const char* deviceIdentifier, const AudioLevelReport& levelReport)
        : deviceId(deviceIdentifier), report(levelReport) {}

    void serialize(JsonDocument& document) const override {
        document["type"] = "audio_level";
        document["deviceId"] = deviceId;
        document["status"] = Inmp441Microphone::describeStatus(report.status);
        document["rmsDbfs"] = round1(report.rmsInDbfs);
        document["peakDbfs"] = round1(report.peakInDbfs);
        document["peak"] = report.peakAmplitude;
        document["noiseFloorDbfs"] = round1(report.noiseFloorInDbfs);
        document["voice"] = report.voiceActive;
        document["clipped"] = report.clippedSamples;
        document["frames"] = report.capturedFrames;
        document["dropped"] = report.droppedFrames;
        document["sampleRate"] = static_cast<int>(report.measuredSampleRateInHertz + 0.5f);
        document["slot"] = report.activeSlot;
        document["gainDb"] = round1(report.gainInDecibels);
        document["windowMs"] = report.windowInMilliseconds;
    }

    const char* deviceId;
    AudioLevelReport report;

private:
    static float round1(float value) { return roundf(value * 10.0f) / 10.0f; }
};

/** @brief Voice activity transition (sound started / ended). */
class VoiceActivityTelemetry : public TelemetryPackage {
public:
    VoiceActivityTelemetry(const char* deviceIdentifier, bool isActive, float levelInDbfs)
        : deviceId(deviceIdentifier), active(isActive), level(levelInDbfs), timestamp(millis()) {}

    void serialize(JsonDocument& document) const override {
        document["type"] = "voice_activity";
        document["deviceId"] = deviceId;
        document["active"] = active;
        document["levelDbfs"] = roundf(level * 10.0f) / 10.0f;
        document["uptimeMs"] = timestamp;
    }

    const char* deviceId;
    bool active;
    float level;
    unsigned long timestamp;
};

#endif // AUDIO_TELEMETRY_H
