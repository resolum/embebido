#include "Inmp441Microphone.h"

#include <cmath>
#include <driver/gpio.h>

/*
 * This file extends the Modest IoT Nano-framework (C++ Edition) with an I2S microphone.
 * Licensed under the MIT License, like the framework.
 */

Inmp441Microphone::Inmp441Microphone(int sckGpioPin, int wsGpioPin, int sdGpioPin,
                                     uint32_t sampleRateInHertz, EventHandler* parentHandler)
    : Sensor(sdGpioPin, parentHandler),
      sckPin(sckGpioPin),
      wsPin(wsGpioPin),
      sampleRate(sampleRateInHertz) {
    setGainInDecibels(0.0f);
}

Inmp441Microphone::~Inmp441Microphone() {
    running = false;
    if (captureTaskHandle != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(50));  // let the capture task leave its read
        captureTaskHandle = nullptr;
    }
    i2s.end();
    if (frameQueue != nullptr) {
        vQueueDelete(frameQueue);
        frameQueue = nullptr;
    }
}

bool Inmp441Microphone::begin(Channel channel, UBaseType_t captureTaskPriority, BaseType_t captureTaskCore) {
    if (running) {
        return true;
    }
    channelMode = channel;
    activeSlot = (channel == Channel::LEFT) ? 0 : (channel == Channel::RIGHT) ? 1 : -1;

    // setPins(bclk, ws, dout, din): dout = -1 because the INMP441 is input-only.
    i2s.setPins(sckPin, wsPin, -1, pin);
    i2s.setTimeout(100);

    // The INMP441 sends 24-bit samples, MSB first, inside 32-bit slots (64 clocks per frame).
    // Both slots are read so the driver works whether L/R is tied to GND or to 3.3V, and so
    // it is not affected by the slot order of the ESP32's mono mode.
    if (!i2s.begin(I2S_MODE_STD, sampleRate, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)) {
        return false;
    }

    // The INMP441 releases SD (high impedance) during the slot it does not own. A pull-down makes
    // that slot read as clean zeros instead of noise, which keeps the AUTO slot detection reliable.
    gpio_pulldown_en(static_cast<gpio_num_t>(pin));

    warmUpFramesLeft = (WARM_UP_MILLISECONDS * sampleRate) / (1000UL * FRAME_SAMPLES) + 1;
    dcPreviousInput = 0.0f;
    dcPreviousOutput = 0.0f;
    reportWindowStart = millis();
    lastTickTimestamp = reportWindowStart;

    running = true;
    const BaseType_t created = xTaskCreatePinnedToCore(&Inmp441Microphone::captureTaskEntry, "MicCapture",
                                                       4096, this, captureTaskPriority,
                                                       &captureTaskHandle, captureTaskCore);
    if (created != pdPASS) {
        running = false;
        i2s.end();
        return false;
    }
    return true;
}

void Inmp441Microphone::captureTaskEntry(void* taskParameters) {
    static_cast<Inmp441Microphone*>(taskParameters)->captureLoop();
    vTaskDelete(nullptr);
}

void Inmp441Microphone::captureLoop() {
    const size_t bytesPerFrame = sizeof(stereoBuffer);

    while (running) {
        size_t bytesReceived = 0;
        while (running && bytesReceived < bytesPerFrame) {
            const size_t justRead = i2s.readBytes(reinterpret_cast<char*>(stereoBuffer) + bytesReceived,
                                                  bytesPerFrame - bytesReceived);
            if (justRead == 0) {
                break;  // timeout: no clock or driver stopped
            }
            bytesReceived += justRead;
        }
        if (bytesReceived < bytesPerFrame) {
            vTaskDelay(1);  // partial frame is discarded; avoid a tight loop if I2S is dead
            continue;
        }

        if (warmUpFramesLeft > 0) {
            --warmUpFramesLeft;  // the INMP441 outputs garbage while it powers up
            continue;
        }

        const int slot = chooseSlot(stereoBuffer, FRAME_SAMPLES);
        convertFrame(stereoBuffer, FRAME_SAMPLES, slot < 0 ? 0 : slot);
    }
}

int Inmp441Microphone::chooseSlot(const int32_t* stereoWords, size_t frameCount) {
    // Variation per slot: sum of |difference| between consecutive 24-bit samples.
    // A connected microphone always has some (its own noise); a pulled-down slot has none.
    uint32_t variation[2] = {0, 0};
    bool allZero = true;
    for (size_t i = 1; i < frameCount; ++i) {
        for (int s = 0; s < 2; ++s) {
            const int32_t current = stereoWords[i * 2 + s] >> 8;
            const int32_t previous = stereoWords[(i - 1) * 2 + s] >> 8;
            const uint32_t delta = static_cast<uint32_t>(abs(current - previous));
            variation[s] = (variation[s] > UINT32_MAX - delta) ? UINT32_MAX : variation[s] + delta;
        }
    }
    for (size_t i = 0; i < frameCount * 2 && allZero; ++i) {
        allZero = stereoWords[i] == 0;
    }

    int slot = activeSlot;
    if (channelMode == Channel::AUTO && !allZero) {
        if (slot < 0) {
            slot = variation[1] > variation[0] ? 1 : 0;
        } else {
            const int other = 1 - slot;
            // Switch only after 0.5 s of the other slot being clearly livelier (hysteresis).
            if (variation[other] > variation[slot] * 8 + 64) {
                if (++framesFavouringOther >= 25) {
                    slot = other;
                    framesFavouringOther = 0;
                }
            } else {
                framesFavouringOther = 0;
            }
        }
        activeSlot = slot;
    }

    portENTER_CRITICAL(&lock);
    if (allZero) {
        window.zeroFrames++;
    } else if (slot >= 0 && variation[slot] == 0) {
        window.stuckFrames++;
    }
    portEXIT_CRITICAL(&lock);
    return slot;
}

void Inmp441Microphone::convertFrame(const int32_t* stereoWords, size_t frameCount, int slot) {
    portENTER_CRITICAL(&lock);
    const float gain = gainLinear;
    portEXIT_CRITICAL(&lock);

    // 24-bit full scale (8388608) to 16-bit full scale (32768) is a factor of 1/256.
    const float scale = gain / 256.0f;
    uint64_t sumOfSquares = 0;
    int peak = 0;
    uint32_t clipped = 0;

    for (size_t i = 0; i < frameCount; ++i) {
        const float sample24 = static_cast<float>(stereoWords[i * 2 + slot] >> 8);

        // One-pole high-pass filter that removes the microphone's DC offset.
        const float filtered = sample24 - dcPreviousInput + DC_FILTER_POLE * dcPreviousOutput;
        dcPreviousInput = sample24;
        dcPreviousOutput = filtered;

        int32_t value = static_cast<int32_t>(lrintf(filtered * scale));
        if (value > 32767) {
            value = 32767;
            ++clipped;
        } else if (value < -32767) {
            value = -32767;
            ++clipped;
        }
        pcmFrame[i] = static_cast<int16_t>(value);

        const int magnitude = abs(value);
        if (magnitude > peak) peak = magnitude;
        sumOfSquares += static_cast<uint64_t>(static_cast<int64_t>(value) * value);
    }

    bool dropped = false;
    if (frameQueue != nullptr && xQueueSend(frameQueue, pcmFrame, 0) != pdPASS) {
        // The consumer is late: drop the OLDEST frame, recent audio matters more.
        static int16_t discarded[FRAME_SAMPLES];
        xQueueReceive(frameQueue, discarded, 0);
        xQueueSend(frameQueue, pcmFrame, 0);
        dropped = true;
    }

    portENTER_CRITICAL(&lock);
    window.sumOfSquares += sumOfSquares;
    window.sampleCount += frameCount;
    if (peak > window.peakAmplitude) window.peakAmplitude = peak;
    window.clippedSamples += clipped;
    window.capturedFrames++;
    if (dropped) window.droppedFrames++;

    if (recordBuffer != nullptr && recordCount < recordCapacity) {
        const size_t toCopy = min(frameCount, recordCapacity - recordCount);
        memcpy(recordBuffer + recordCount, pcmFrame, toCopy * sizeof(int16_t));
        recordCount += toCopy;
    }
    portEXIT_CRITICAL(&lock);
}

void Inmp441Microphone::processEvent(Event& event) {
    if (event.identifier != MEASURE_DATA_REQUESTED_EVENT_IDENTIFIER) {
        return;
    }
    const unsigned long now = millis();

    CaptureWindow tick;
    portENTER_CRITICAL(&lock);
    tick = window;
    window = CaptureWindow();
    portEXIT_CRITICAL(&lock);
    lastTickTimestamp = now;

    const float tickLevel = tick.sampleCount > 0
        ? toDbfs(sqrtf(static_cast<float>(tick.sumOfSquares / tick.sampleCount)))
        : -120.0f;

    portENTER_CRITICAL(&lock);
    instantLevelInDbfs = tickLevel;
    portEXIT_CRITICAL(&lock);

    // --- Noise floor and voice-activity detection (energy with hysteresis and hangover) ---
    if (tick.sampleCount > 0 && tick.zeroFrames < tick.capturedFrames) {
        if (!noiseFloorReady) {
            noiseFloorInDbfs = tickLevel;
            noiseFloorReady = true;
        } else if (tickLevel < noiseFloorInDbfs) {
            noiseFloorInDbfs += (tickLevel - noiseFloorInDbfs) * NOISE_FLOOR_FALL_RATE;
        } else if (!voiceActive) {
            noiseFloorInDbfs += (tickLevel - noiseFloorInDbfs) * NOISE_FLOOR_RISE_RATE;
        }

        const float threshold = fmaxf(noiseFloorInDbfs + voiceThresholdInDecibels, MINIMUM_VOICE_LEVEL_IN_DBFS);
        if (!voiceActive) {
            if (tickLevel > threshold) {
                voiceActive = true;
                voiceStatePending = true;
                belowThresholdSince = 0;
            }
        } else if (tickLevel < threshold - 3.0f) {
            if (belowThresholdSince == 0) {
                belowThresholdSince = now;
            } else if (now - belowThresholdSince >= voiceHangoverInMilliseconds) {
                voiceActive = false;
                voiceStatePending = true;
            }
        } else {
            belowThresholdSince = 0;
        }
    }

    // --- Accumulate the tick into the report window ---
    reportWindow.sumOfSquares += tick.sumOfSquares;
    reportWindow.sampleCount += tick.sampleCount;
    if (tick.peakAmplitude > reportWindow.peakAmplitude) reportWindow.peakAmplitude = tick.peakAmplitude;
    reportWindow.clippedSamples += tick.clippedSamples;
    reportWindow.capturedFrames += tick.capturedFrames;
    reportWindow.droppedFrames += tick.droppedFrames;
    reportWindow.zeroFrames += tick.zeroFrames;
    reportWindow.stuckFrames += tick.stuckFrames;

    if (voiceStatePending) {
        voiceStatePending = false;
        event = Event(voiceActive ? SOUND_STARTED_EVENT_IDENTIFIER : SOUND_ENDED_EVENT_IDENTIFIER, pin);
        return;  // the report (if due) goes out on the next tick
    }

    const unsigned long elapsed = now - reportWindowStart;
    if (elapsed < reportIntervalInMilliseconds) {
        event.identifier = PROPAGATION_SUPPRESSION_IDENTIFIER;
        return;
    }

    AudioLevelReport report;
    const CaptureWindow& w = reportWindow;
    report.windowInMilliseconds = elapsed;
    report.capturedFrames = w.capturedFrames;
    report.droppedFrames = w.droppedFrames;
    report.clippedSamples = w.clippedSamples;
    report.peakAmplitude = w.peakAmplitude;
    report.peakInDbfs = toDbfs(static_cast<float>(w.peakAmplitude));
    report.rmsInDbfs = w.sampleCount > 0 ? toDbfs(sqrtf(static_cast<float>(w.sumOfSquares / w.sampleCount))) : -120.0f;
    report.noiseFloorInDbfs = noiseFloorReady ? noiseFloorInDbfs : -120.0f;
    report.measuredSampleRateInHertz = elapsed > 0 ? (w.sampleCount * 1000.0f) / elapsed : 0.0f;
    report.activeSlot = activeSlot;
    report.voiceActive = voiceActive;
    report.gainInDecibels = getGainInDecibels();

    if (w.capturedFrames == 0) {
        report.status = warmUpFramesLeft > 0 ? MicrophoneSignalStatus::STARTING : MicrophoneSignalStatus::NO_SAMPLES;
    } else if (w.zeroFrames * 10 >= w.capturedFrames * 9) {
        report.status = MicrophoneSignalStatus::ONLY_ZEROS;
    } else if (w.stuckFrames * 10 >= w.capturedFrames * 9) {
        report.status = MicrophoneSignalStatus::STUCK_VALUE;
    } else if (w.clippedSamples * 100 > w.sampleCount) {
        report.status = MicrophoneSignalStatus::SATURATED;
    } else {
        report.status = MicrophoneSignalStatus::OK;
    }

    portENTER_CRITICAL(&lock);
    lastReport = report;
    portEXIT_CRITICAL(&lock);

    reportWindow = CaptureWindow();
    reportWindowStart = now;
    event = Event(DATA_READ_EVENT_IDENTIFIER, pin);
}

void Inmp441Microphone::setGainInDecibels(float decibels) {
    const float clamped = fminf(fmaxf(decibels, 0.0f), 48.0f);
    const float linear = powf(10.0f, clamped / 20.0f);
    portENTER_CRITICAL(&lock);
    gainInDecibels = clamped;
    gainLinear = linear;
    portEXIT_CRITICAL(&lock);
}

float Inmp441Microphone::getGainInDecibels() const {
    portENTER_CRITICAL(&lock);
    const float value = gainInDecibels;
    portEXIT_CRITICAL(&lock);
    return value;
}

void Inmp441Microphone::setVoiceThresholdInDecibels(float decibelsAboveNoiseFloor) {
    voiceThresholdInDecibels = decibelsAboveNoiseFloor;
}

void Inmp441Microphone::setVoiceHangoverInMilliseconds(unsigned long hangoverInMilliseconds) {
    voiceHangoverInMilliseconds = hangoverInMilliseconds;
}

void Inmp441Microphone::setReportIntervalInMilliseconds(unsigned long intervalInMilliseconds) {
    reportIntervalInMilliseconds = intervalInMilliseconds;
}

AudioLevelReport Inmp441Microphone::getLastReport() const {
    portENTER_CRITICAL(&lock);
    const AudioLevelReport copy = lastReport;
    portEXIT_CRITICAL(&lock);
    return copy;
}

float Inmp441Microphone::getInstantLevelInDbfs() const {
    portENTER_CRITICAL(&lock);
    const float value = instantLevelInDbfs;
    portEXIT_CRITICAL(&lock);
    return value;
}

bool Inmp441Microphone::isVoiceActive() const { return voiceActive; }

uint32_t Inmp441Microphone::getSampleRate() const { return sampleRate; }

int Inmp441Microphone::getActiveSlot() const { return activeSlot; }

bool Inmp441Microphone::startRecording(int16_t* buffer, size_t sampleCapacity) {
    if (buffer == nullptr || sampleCapacity == 0) return false;
    bool started = false;
    portENTER_CRITICAL(&lock);
    if (recordBuffer == nullptr || recordCount >= recordCapacity) {
        recordBuffer = buffer;
        recordCapacity = sampleCapacity;
        recordCount = 0;
        started = true;
    }
    portEXIT_CRITICAL(&lock);
    return started;
}

bool Inmp441Microphone::isRecording() const {
    portENTER_CRITICAL(&lock);
    const bool recording = recordBuffer != nullptr && recordCount < recordCapacity;
    portEXIT_CRITICAL(&lock);
    return recording;
}

size_t Inmp441Microphone::getRecordedSampleCount() const {
    portENTER_CRITICAL(&lock);
    const size_t count = recordCount;
    portEXIT_CRITICAL(&lock);
    return count;
}

void Inmp441Microphone::cancelRecording() {
    portENTER_CRITICAL(&lock);
    recordBuffer = nullptr;
    recordCapacity = 0;
    recordCount = 0;
    portEXIT_CRITICAL(&lock);
}

bool Inmp441Microphone::enableFrameQueue(size_t depthInFrames) {
    if (frameQueue != nullptr) return true;
    if (depthInFrames == 0) return false;
    frameQueue = xQueueCreate(depthInFrames, sizeof(int16_t) * FRAME_SAMPLES);
    return frameQueue != nullptr;
}

bool Inmp441Microphone::readFrame(int16_t* destination, TickType_t waitTicks) {
    if (frameQueue == nullptr || destination == nullptr) return false;
    return xQueueReceive(frameQueue, destination, waitTicks) == pdPASS;
}

float Inmp441Microphone::toDbfs(float amplitude) {
    if (amplitude <= 0.0f) return -120.0f;
    return 20.0f * log10f(amplitude / 32768.0f);
}

const char* Inmp441Microphone::describeStatus(MicrophoneSignalStatus status) {
    switch (status) {
        case MicrophoneSignalStatus::STARTING:    return "INICIANDO";
        case MicrophoneSignalStatus::OK:          return "OK";
        case MicrophoneSignalStatus::NO_SAMPLES:  return "SIN_DATOS";
        case MicrophoneSignalStatus::ONLY_ZEROS:  return "SOLO_CEROS";
        case MicrophoneSignalStatus::STUCK_VALUE: return "VALOR_FIJO";
        case MicrophoneSignalStatus::SATURATED:   return "SATURADO";
    }
    return "?";
}

const char* Inmp441Microphone::describeStatusHint(MicrophoneSignalStatus status) {
    switch (status) {
        case MicrophoneSignalStatus::STARTING:
            return "El microfono esta arrancando (300 ms).";
        case MicrophoneSignalStatus::OK:
            return "Llega audio real.";
        case MicrophoneSignalStatus::NO_SAMPLES:
            return "I2S no entrega datos: revisa SCK y WS, y que el core ESP32 sea 3.x.";
        case MicrophoneSignalStatus::ONLY_ZEROS:
            return "Solo ceros: revisa SD, VDD a 3.3V, GND, y que L/R este conectado a GND (no al aire).";
        case MicrophoneSignalStatus::STUCK_VALUE:
            return "SD no cambia nunca: puede estar en corto con 3.3V o GND, o el pin SD esta mal en la config.";
        case MicrophoneSignalStatus::SATURATED:
            return "Mas del 1% de muestras recortan: baja la ganancia con '-'.";
    }
    return "";
}
