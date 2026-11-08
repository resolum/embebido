#ifndef AUDIO_STREAMER_H
#define AUDIO_STREAMER_H

/**
 * @file AudioStreamer.h
 * @brief Microphone uplink: drains the frame queue of the INMP441 and sends the PCM to the edge.
 *
 * Runs in its own task on core 0 (where WiFi lives) with a lower priority than the link and the playback.
 * Frames are grouped (STREAM_FRAMES_PER_MESSAGE x 640 B) into one binary WebSocket message. With no
 * link the audio is thrown away and counted, never accumulated: the queue must not fill with stale audio.
 */

#include "EdgeLink.h"
#include "Inmp441Microphone.h"

class AudioStreamer {
public:
    AudioStreamer(Inmp441Microphone& microphoneSensor, EdgeLink& edgeLink);

    /** @brief Starts the task. The microphone frame queue must already be enabled. */
    bool begin();

private:
    static void taskEntry(void* parameters);
    void run();

    Inmp441Microphone& microphone;
    EdgeLink& link;
    TaskHandle_t taskHandle = nullptr;
};

#endif // AUDIO_STREAMER_H
