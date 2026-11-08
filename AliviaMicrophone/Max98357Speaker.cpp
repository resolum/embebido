#include "Max98357Speaker.h"

#include <esp_heap_caps.h>
#include <math.h>

#include "SerialConsole.h"

namespace {
// ESP_I2S creates 6 DMA descriptors of 240 frames: ~90 ms at 16 kHz sit in the hardware ahead of write().
constexpr size_t DMA_DEPTH_SAMPLES = 6 * 240;
constexpr unsigned long AUDIO_NEVER_ARRIVED_MS = 3000;
constexpr unsigned long UNDERRUN_GIVE_UP_MS = 4000;
}  // namespace

Max98357Speaker::Max98357Speaker(int bclkPin, int lrcPin, int dinPin, uint32_t sampleRateInHertz,
                                 EdgeLink& edgeLink, CommandHandler* parentHandler)
    : Actuator(dinPin, parentHandler),
      bclk(bclkPin),
      lrc(lrcPin),
      din(dinPin),
      sampleRate(sampleRateInHertz),
      link(edgeLink) {}

Max98357Speaker::~Max98357Speaker() {
    if (taskHandle != nullptr) vTaskDelete(taskHandle);
    if (ring != nullptr) heap_caps_free(ring);
}

bool Max98357Speaker::begin() {
    // setPins(bclk, ws, dout, din): the amplifier is output-only, so din = -1.
    i2s.setPins(bclk, lrc, din, -1);
    i2s.setTimeout(100);
    // Mono 16 bit; I2S_STD_SLOT_BOTH repeats the sample on both slots so the amplifier outputs full level
    // whatever channel its SD pin selects. The microphone owns the other I2S port.
    if (!i2s.begin(I2S_MODE_STD, sampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_BOTH)) {
        return false;
    }

    ringCapacity = (static_cast<size_t>(sampleRate) * 2 * SPEAKER_BUFFER_MS / 1000) & ~static_cast<size_t>(1);
    ring = static_cast<uint8_t*>(heap_caps_malloc(ringCapacity, MALLOC_CAP_8BIT));
    if (ring == nullptr) {
        i2s.end();
        return false;
    }
    counters.bufferCapacityBytes = ringCapacity;

    return xTaskCreatePinnedToCore(&Max98357Speaker::taskEntry, "Playback", PLAYBACK_TASK_STACK_BYTES, this,
                                   PLAYBACK_TASK_PRIORITY, &taskHandle, 0) == pdPASS;
}

void Max98357Speaker::setVolumePercent(uint8_t percent) { volumePercent = percent > 100 ? 100 : percent; }

uint8_t Max98357Speaker::getVolumePercent() const { return volumePercent; }

SpeakerStats Max98357Speaker::stats() const {
    portENTER_CRITICAL(&ringLock);
    SpeakerStats copy = counters;
    copy.bufferedBytes = ringCount;
    copy.playing = active;
    portEXIT_CRITICAL(&ringLock);
    return copy;
}

// ---------------------------------------------------------------------------
// Jitter buffer (callers hold ringLock, except stats helpers)
// ---------------------------------------------------------------------------

size_t Max98357Speaker::ringSize() const { return ringCount; }

size_t Max98357Speaker::ringPush(const uint8_t* data, size_t length) {
    const size_t room = ringCapacity - ringCount;
    const size_t accepted = length < room ? length : room;
    for (size_t i = 0; i < accepted; ++i) {
        ring[ringHead] = data[i];
        ringHead = (ringHead + 1) % ringCapacity;
    }
    ringCount += accepted;
    return accepted;
}

size_t Max98357Speaker::ringPop(uint8_t* destination, size_t maximum) {
    const size_t taken = (maximum < ringCount ? maximum : ringCount) & ~static_cast<size_t>(1);
    for (size_t i = 0; i < taken; ++i) {
        destination[i] = ring[ringTail];
        ringTail = (ringTail + 1) % ringCapacity;
    }
    ringCount -= taken;
    return taken;
}

// ---------------------------------------------------------------------------
// EdgeLinkListener: runs in the link task, must be quick
// ---------------------------------------------------------------------------

void Max98357Speaker::onAudioBegin(const char* id, uint32_t rate, bool emergency) {
    if (rate != sampleRate) {
        SerialConsole::printf("[Parlante] audio_begin a %lu Hz pero el parlante anuncio %lu Hz: se ignora\n",
                              static_cast<unsigned long>(rate), static_cast<unsigned long>(sampleRate));
        return;
    }
    portENTER_CRITICAL(&ringLock);
    ringHead = ringTail = ringCount = 0;
    strlcpy(utteranceId, id, sizeof(utteranceId));
    generation++;
    active = true;
    ended = false;
    counters.utterances++;
    portEXIT_CRITICAL(&ringLock);
    if (emergency) SerialConsole::printf("[Parlante] audio de emergencia\n");
}

void Max98357Speaker::onAudioData(const uint8_t* pcm, size_t length) {
    portENTER_CRITICAL(&ringLock);
    if (active && !ended) {
        const size_t wanted = length & ~static_cast<size_t>(1);  // whole samples only
        const size_t accepted = ringPush(pcm, wanted);
        counters.overflowBytes += static_cast<uint32_t>(wanted - accepted);
    }
    portEXIT_CRITICAL(&ringLock);
}

void Max98357Speaker::onAudioEnd(const char* id) {
    portENTER_CRITICAL(&ringLock);
    if (active && strcmp(id, utteranceId) == 0) ended = true;
    portEXIT_CRITICAL(&ringLock);
}

void Max98357Speaker::onAudioCancel(const char* id) {
    portENTER_CRITICAL(&ringLock);
    if (active && (id[0] == '\0' || strcmp(id, utteranceId) == 0)) {
        active = false;
        ringHead = ringTail = ringCount = 0;
        counters.cancelled++;
    }
    portEXIT_CRITICAL(&ringLock);
}

void Max98357Speaker::onLinkDown() {
    portENTER_CRITICAL(&ringLock);
    if (active) counters.cancelled++;
    active = false;
    ringHead = ringTail = ringCount = 0;
    portEXIT_CRITICAL(&ringLock);
    stopRequested = true;
}

void Max98357Speaker::stopNow() {
    portENTER_CRITICAL(&ringLock);
    active = false;
    ringHead = ringTail = ringCount = 0;
    portEXIT_CRITICAL(&ringLock);
    stopRequested = true;
}

void Max98357Speaker::executeCommand(Command command) {
    if (command.identifier == PLAY_TEST_TONE_COMMAND_IDENTIFIER) {
        toneRequested = true;
    } else if (command.identifier == STOP_COMMAND_IDENTIFIER) {
        stopNow();
    }
}

// ---------------------------------------------------------------------------
// Playback task (core 0)
// ---------------------------------------------------------------------------

void Max98357Speaker::taskEntry(void* parameters) { static_cast<Max98357Speaker*>(parameters)->run(); }

void Max98357Speaker::run() {
    for (;;) {
        portENTER_CRITICAL(&ringLock);
        const bool utterancePending = active;
        portEXIT_CRITICAL(&ringLock);

        if (utterancePending) {
            playUtterance();
        } else if (toneRequested) {
            toneRequested = false;
            playTone();
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void Max98357Speaker::writeSamples(int16_t* samples, size_t count) {
    const int32_t gain = (static_cast<int32_t>(volumePercent) * 256) / 100;
    for (size_t i = 0; i < count; ++i) {
        samples[i] = static_cast<int16_t>((static_cast<int32_t>(samples[i]) * gain) >> 8);
    }
    i2s.write(reinterpret_cast<const uint8_t*>(samples), count * sizeof(int16_t));
}

void Max98357Speaker::writeSilence(size_t samples) {
    static int16_t silence[CHUNK_SAMPLES] = {};
    while (samples > 0) {
        const size_t part = samples < CHUNK_SAMPLES ? samples : CHUNK_SAMPLES;
        i2s.write(reinterpret_cast<const uint8_t*>(silence), part * sizeof(int16_t));
        samples -= part;
    }
}

void Max98357Speaker::playUtterance() {
    portENTER_CRITICAL(&ringLock);
    const uint32_t myGeneration = generation;
    char myId[ID_CAPACITY];
    strlcpy(myId, utteranceId, sizeof(myId));
    portEXIT_CRITICAL(&ringLock);

    const size_t prefillBytes = static_cast<size_t>(sampleRate) * 2 * SPEAKER_PREFILL_MS / 1000;

    // Wait for the prefill (or for the end of a short utterance) before the first sample.
    // The same wait is used to re-buffer after an underrun, with half the prefill.
    auto buffered = [&](size_t needed, unsigned long timeoutMs) -> int {
        const unsigned long startedAt = millis();
        for (;;) {
            portENTER_CRITICAL(&ringLock);
            const bool stillMine = active && generation == myGeneration;
            const bool finished = ended;
            const size_t bytes = ringCount;
            portEXIT_CRITICAL(&ringLock);
            if (!stillMine) return -1;
            if (bytes >= needed || finished) return 1;
            if (millis() - startedAt > timeoutMs) return 0;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    };

    stopRequested = false;
    if (buffered(prefillBytes, AUDIO_NEVER_ARRIVED_MS) != 1) {
        SerialConsole::printf("[Parlante] el audio no llego completo: se descarta\n");
        stopNow();
        return;
    }

    for (;;) {
        portENTER_CRITICAL(&ringLock);
        const bool stillMine = active && generation == myGeneration;
        const bool finished = ended;
        const size_t got = stillMine ? ringPop(reinterpret_cast<uint8_t*>(chunk), sizeof(chunk)) : 0;
        portEXIT_CRITICAL(&ringLock);

        if (!stillMine || stopRequested) {
            stopRequested = false;
            return;  // cancelled or replaced: whatever the DMA still holds (<= 90 ms) plays out
        }
        if (got >= sizeof(int16_t)) {
            writeSamples(chunk, got / sizeof(int16_t));
            continue;
        }
        if (finished) {
            // Push the last samples through the DMA so playback_done means "it already sounded".
            writeSilence(DMA_DEPTH_SAMPLES);
            portENTER_CRITICAL(&ringLock);
            const bool stillCurrent = active && generation == myGeneration;
            active = false;
            portEXIT_CRITICAL(&ringLock);
            if (stillCurrent) link.sendPlaybackDone(myId);
            return;
        }

        // Underrun: the DMA clears itself (silence). Re-buffer instead of stuttering.
        portENTER_CRITICAL(&ringLock);
        counters.underruns++;
        portEXIT_CRITICAL(&ringLock);
        if (buffered(prefillBytes / 2, UNDERRUN_GIVE_UP_MS) == 0) {
            SerialConsole::printf("[Parlante] el audio se interrumpio: se descarta\n");
            stopNow();
            return;
        }
    }
}

void Max98357Speaker::playTone() {
    const float amplitude = 0.3f * 32767.0f;
    const float step = 2.0f * static_cast<float>(M_PI) * TEST_TONE_HZ / static_cast<float>(sampleRate);
    float phase = 0.0f;
    const size_t totalSamples = static_cast<size_t>(sampleRate) * TEST_TONE_MS / 1000;
    stopRequested = false;

    SerialConsole::printf("[Parlante] tono de prueba %lu Hz, %lu ms, volumen %u%%\n",
                          static_cast<unsigned long>(TEST_TONE_HZ), static_cast<unsigned long>(TEST_TONE_MS),
                          static_cast<unsigned>(volumePercent));
    for (size_t done = 0; done < totalSamples && !stopRequested; done += CHUNK_SAMPLES) {
        portENTER_CRITICAL(&ringLock);
        const bool utteranceArrived = active;
        portEXIT_CRITICAL(&ringLock);
        if (utteranceArrived) break;  // real audio wins over the test tone

        const size_t count = min(CHUNK_SAMPLES, totalSamples - done);
        for (size_t i = 0; i < count; ++i) {
            chunk[i] = static_cast<int16_t>(amplitude * sinf(phase));
            phase += step;
            if (phase > 2.0f * static_cast<float>(M_PI)) phase -= 2.0f * static_cast<float>(M_PI);
        }
        writeSamples(chunk, count);
    }
    stopRequested = false;
    writeSilence(DMA_DEPTH_SAMPLES);
}
