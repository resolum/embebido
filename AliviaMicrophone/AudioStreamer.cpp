#include "AudioStreamer.h"

#include "MicrophoneConfig.h"

AudioStreamer::AudioStreamer(Inmp441Microphone& microphoneSensor, EdgeLink& edgeLink)
    : microphone(microphoneSensor), link(edgeLink) {}

bool AudioStreamer::begin() {
    return xTaskCreatePinnedToCore(&AudioStreamer::taskEntry, "MicStream", STREAM_TASK_STACK_BYTES, this,
                                   STREAM_TASK_PRIORITY, &taskHandle, 0) == pdPASS;
}

void AudioStreamer::taskEntry(void* parameters) { static_cast<AudioStreamer*>(parameters)->run(); }

void AudioStreamer::run() {
    constexpr size_t frameSamples = Inmp441Microphone::FRAME_SAMPLES;
    constexpr size_t framesPerMessage = STREAM_FRAMES_PER_MESSAGE < 1 ? 1 : (STREAM_FRAMES_PER_MESSAGE > 4 ? 4 : STREAM_FRAMES_PER_MESSAGE);
    static int16_t message[frameSamples * framesPerMessage];
    size_t framesInMessage = 0;

    for (;;) {
        if (!microphone.readFrame(message + framesInMessage * frameSamples, pdMS_TO_TICKS(40))) {
            continue;
        }

        if (!link.isReady()) {
            // No link: the frame is dropped, and so is any half-built message (it would be stale).
            framesInMessage = 0;
            link.countMicFrames(0, 1, 0);
            continue;
        }

        if (++framesInMessage < framesPerMessage) continue;
        framesInMessage = 0;

        if (link.sendBinary(reinterpret_cast<const uint8_t*>(message), sizeof(message))) {
            link.countMicFrames(framesPerMessage, 0, 0);
        } else {
            link.countMicFrames(0, 0, framesPerMessage);
        }
    }
}
