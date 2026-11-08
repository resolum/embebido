#ifndef EDGE_LINK_H
#define EDGE_LINK_H

/**
 * @file EdgeLink.h
 * @brief WiFi + WebSocket client to the Alivia voice edge (see docs/protocolo-websocket.md).
 *
 * One task on core 0 owns the connection: it keeps WiFi up, opens ws://<host>:<port>/ws, sends the
 * "hello", waits for "ready" and reconnects with exponential backoff (1 s ... 10 s). Other tasks send
 * through sendBinary()/sendText(), which are serialised with a mutex and fail fast when the link is down.
 *
 * Incoming speaker messages (audio_begin / PCM / audio_end / audio_cancel) are handed to an
 * EdgeLinkListener; onLinkDown() tells it to cut playback and flush its buffers.
 */

#include <Arduino.h>
#include <WebSocketsClient.h>

#include "MicrophoneConfig.h"

class EdgeLinkListener {
public:
    virtual ~EdgeLinkListener() = default;
    virtual void onAudioBegin(const char* utteranceId, uint32_t sampleRate, bool emergency) = 0;
    virtual void onAudioData(const uint8_t* pcm, size_t length) = 0;
    virtual void onAudioEnd(const char* utteranceId) = 0;
    virtual void onAudioCancel(const char* utteranceId) = 0;
    /** @brief The connection dropped (or never came up): stop sound and empty buffers. */
    virtual void onLinkDown() = 0;
};

enum class LinkState : uint8_t { WIFI_DOWN, DISCONNECTED, CONNECTING, READY };

struct LinkStats {
    uint32_t bytesSent = 0;
    uint32_t bytesReceived = 0;
    uint32_t micFramesSent = 0;
    uint32_t micFramesDroppedNoLink = 0;  ///< Mic frames thrown away because the link was not ready.
    uint32_t micFramesDroppedSendFail = 0;
    uint32_t micQueueDrops = 0;           ///< Oldest frames evicted by the mic queue (consumer late).
    uint32_t reconnects = 0;
    uint32_t handshakeFailures = 0;
};

class EdgeLink {
public:
    EdgeLink(const char* ssid, const char* password, const char* host, uint16_t port, const char* path,
             const char* deviceId, const char* token);

    void setListener(EdgeLinkListener* newListener);

    /** @brief Starts WiFi and the link task (core 0). */
    bool begin();

    [[nodiscard]] bool isReady() const;
    [[nodiscard]] LinkState state() const;
    static const char* describeState(LinkState state);

    /** @brief Sends a binary message (microphone PCM). False when the link is not ready or the lock is busy. */
    bool sendBinary(const uint8_t* data, size_t length);
    bool sendText(const char* json);
    bool sendPlaybackDone(const char* utteranceId);

    void countMicFrames(uint32_t sent, uint32_t droppedNoLink, uint32_t droppedSendFail);
    void addMicQueueDrops(uint32_t drops);
    [[nodiscard]] LinkStats stats() const;

    [[nodiscard]] const char* host() const;
    [[nodiscard]] uint16_t port() const;

private:
    static void taskEntry(void* parameters);
    void run();
    void maintainWifi(unsigned long now);
    void tryConnect(unsigned long now);
    void failAttempt(unsigned long now, const char* reason);
    void tearDownSocket();
    void handleEvent(WStype_t type, uint8_t* payload, size_t length);
    void handleText(const uint8_t* payload, size_t length);
    void sendHello();
    void notifyDown();
    void scheduleRetry(unsigned long now);

    const char* ssid;
    const char* password;
    const char* edgeHost;
    const uint16_t edgePort;
    const char* edgePath;
    const char* deviceId;
    const char* token;

    EdgeLinkListener* listener = nullptr;
    WebSocketsClient* socket = nullptr;  ///< A new client per attempt: no stale library state between reconnects.
    SemaphoreHandle_t lock = nullptr;
    TaskHandle_t taskHandle = nullptr;

    volatile LinkState linkState = LinkState::WIFI_DOWN;
    unsigned long stateSince = 0;
    unsigned long nextAttemptAt = 0;
    unsigned long backoffMs = LINK_BACKOFF_MIN_MS;
    unsigned long lastWifiBeginAt = 0;
    volatile bool teardownRequested = false;
    bool announcedDown = true;
    LinkStats counters;
    mutable portMUX_TYPE statsLock = portMUX_INITIALIZER_UNLOCKED;
};

#endif // EDGE_LINK_H
