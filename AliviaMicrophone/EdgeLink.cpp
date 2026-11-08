#include "EdgeLink.h"

#include <ArduinoJson.h>
#include <WiFi.h>

#include "Inmp441Microphone.h"
#include "SerialConsole.h"

namespace {
constexpr unsigned long WIFI_RETRY_MS = 10000;
constexpr unsigned long TCP_PROBE_TIMEOUT_MS = 2000;
constexpr TickType_t SEND_LOCK_WAIT = pdMS_TO_TICKS(15);
}  // namespace

EdgeLink::EdgeLink(const char* wifiSsid, const char* wifiPassword, const char* host, uint16_t port,
                   const char* path, const char* deviceIdentifier, const char* authToken)
    : ssid(wifiSsid),
      password(wifiPassword),
      edgeHost(host),
      edgePort(port),
      edgePath(path),
      deviceId(deviceIdentifier),
      token(authToken) {}

void EdgeLink::setListener(EdgeLinkListener* newListener) { listener = newListener; }

bool EdgeLink::begin() {
    lock = xSemaphoreCreateMutex();
    if (lock == nullptr) return false;

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // modem sleep adds tens of ms of jitter to the audio
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid, password);
    lastWifiBeginAt = millis();
    stateSince = lastWifiBeginAt;

    // Core 0 is where the WiFi stack lives; core 1 belongs to the I2S capture.
    return xTaskCreatePinnedToCore(&EdgeLink::taskEntry, "EdgeLink", LINK_TASK_STACK_BYTES, this,
                                   LINK_TASK_PRIORITY, &taskHandle, 0) == pdPASS;
}

void EdgeLink::taskEntry(void* parameters) { static_cast<EdgeLink*>(parameters)->run(); }

void EdgeLink::run() {
    for (;;) {
        const unsigned long now = millis();
        maintainWifi(now);

        if (WiFi.status() == WL_CONNECTED) {
            if (linkState == LinkState::WIFI_DOWN) {
                linkState = LinkState::DISCONNECTED;
                nextAttemptAt = now;
                SerialConsole::printf("[Link] WiFi conectado: IP %s, RSSI %d dBm\n",
                                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
            }
            if (linkState == LinkState::DISCONNECTED && now >= nextAttemptAt) {
                tryConnect(now);
            }
            if (socket != nullptr) {
                if (xSemaphoreTake(lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                    socket->loop();
                    xSemaphoreGive(lock);
                }
                if (linkState == LinkState::CONNECTING && now - stateSince > LINK_CONNECT_TIMEOUT_MS) {
                    failAttempt(now, "tiempo de espera del handshake");
                }
            }
        } else if (linkState != LinkState::WIFI_DOWN) {
            SerialConsole::printf("[Link] WiFi perdido\n");
            teardownRequested = true;
        }

        if (teardownRequested) {
            teardownRequested = false;
            tearDownSocket();
            notifyDown();
            if (WiFi.status() == WL_CONNECTED) {
                scheduleRetry(millis());
            } else {
                linkState = LinkState::WIFI_DOWN;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void EdgeLink::maintainWifi(unsigned long now) {
    if (WiFi.status() != WL_CONNECTED && now - lastWifiBeginAt > WIFI_RETRY_MS) {
        lastWifiBeginAt = now;
        WiFi.disconnect();
        WiFi.begin(ssid, password);
    }
}

void EdgeLink::tryConnect(unsigned long now) {
    // Probe first: if the PC is unreachable we back off here instead of letting the library
    // retry every 500 ms inside the attempt.
    WiFiClient probe;
    if (!probe.connect(edgeHost, edgePort, TCP_PROBE_TIMEOUT_MS)) {
        failAttempt(now, "el edge no responde por TCP");
        return;
    }
    probe.stop();

    xSemaphoreTake(lock, portMAX_DELAY);
    socket = new WebSocketsClient();
    socket->begin(edgeHost, edgePort, edgePath, "");  // empty subprotocol: the edge does not negotiate one
    socket->onEvent([this](WStype_t type, uint8_t* payload, size_t length) { handleEvent(type, payload, length); });
    socket->enableHeartbeat(LINK_PING_INTERVAL_MS, LINK_PONG_TIMEOUT_MS, LINK_MAX_MISSED_PONGS);
    xSemaphoreGive(lock);

    linkState = LinkState::CONNECTING;
    stateSince = now;
    portENTER_CRITICAL(&statsLock);
    counters.reconnects++;
    portEXIT_CRITICAL(&statsLock);
    SerialConsole::printf("[Link] conectando a ws://%s:%u%s\n", edgeHost, edgePort, edgePath);
}

void EdgeLink::failAttempt(unsigned long now, const char* reason) {
    SerialConsole::printf("[Link] intento fallido (%s); reintento en %lu ms\n", reason, backoffMs);
    portENTER_CRITICAL(&statsLock);
    counters.handshakeFailures++;
    portEXIT_CRITICAL(&statsLock);
    tearDownSocket();
    notifyDown();
    scheduleRetry(now);
}

void EdgeLink::scheduleRetry(unsigned long now) {
    linkState = LinkState::DISCONNECTED;
    nextAttemptAt = now + backoffMs;
    backoffMs = min<unsigned long>(backoffMs * 2, LINK_BACKOFF_MAX_MS);
}

void EdgeLink::tearDownSocket() {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (socket != nullptr) {
        socket->disconnect();
        delete socket;
        socket = nullptr;
    }
    xSemaphoreGive(lock);
}

/** The link was ready and is gone: tell the speaker to cut sound and empty its buffers. */
void EdgeLink::notifyDown() {
    if (announcedDown) return;
    announcedDown = true;
    if (listener != nullptr) listener->onLinkDown();
}

void EdgeLink::handleEvent(WStype_t type, uint8_t* payload, size_t length) {
    // Runs inside socket->loop(), so the lock is already held by the link task.
    switch (type) {
        case WStype_CONNECTED:
            sendHello();
            break;
        case WStype_TEXT:
            portENTER_CRITICAL(&statsLock);
            counters.bytesReceived += length;
            portEXIT_CRITICAL(&statsLock);
            handleText(payload, length);
            break;
        case WStype_BIN:
            portENTER_CRITICAL(&statsLock);
            counters.bytesReceived += length;
            portEXIT_CRITICAL(&statsLock);
            if (linkState == LinkState::READY && listener != nullptr) listener->onAudioData(payload, length);
            break;
        case WStype_DISCONNECTED:
            if (linkState != LinkState::WIFI_DOWN && linkState != LinkState::DISCONNECTED) {
                SerialConsole::printf("[Link] conexion cerrada\n");
                teardownRequested = true;
            }
            break;
        default:
            break;  // ping/pong are answered by the library
    }
}

void EdgeLink::sendHello() {
    JsonDocument document;
    document["type"] = "hello";
    document["deviceId"] = deviceId;
    JsonArray capabilities = document["capabilities"].to<JsonArray>();
    capabilities.add("microphone");
    capabilities.add("speaker");
    document["micSampleRate"] = AUDIO_SAMPLE_RATE;
    document["playbackSampleRate"] = PLAYBACK_SAMPLE_RATE;
    document["bits"] = 16;
    document["channels"] = 1;
    document["frameSamples"] = Inmp441Microphone::FRAME_SAMPLES;
    document["token"] = token;
    String json;
    serializeJson(document, json);
    socket->sendTXT(json);
}

void EdgeLink::handleText(const uint8_t* payload, size_t length) {
    JsonDocument document;
    if (deserializeJson(document, payload, length) != DeserializationError::Ok) return;
    const char* type = document["type"] | "";

    if (strcmp(type, "ready") == 0) {
        linkState = LinkState::READY;
        announcedDown = false;
        backoffMs = LINK_BACKOFF_MIN_MS;
        SerialConsole::printf("[Link] enlace listo con el edge\n");
    } else if (strcmp(type, "error") == 0) {
        SerialConsole::printf("[Link] el edge rechazo el hello: %s\n", document["reason"] | "?");
        teardownRequested = true;  // the teardown applies the backoff
    } else if (listener == nullptr || linkState != LinkState::READY) {
        return;
    } else if (strcmp(type, "audio_begin") == 0) {
        listener->onAudioBegin(document["utteranceId"] | "", document["sampleRate"] | PLAYBACK_SAMPLE_RATE,
                               strcmp(document["priority"] | "normal", "emergency") == 0);
    } else if (strcmp(type, "audio_end") == 0) {
        listener->onAudioEnd(document["utteranceId"] | "");
    } else if (strcmp(type, "audio_cancel") == 0) {
        listener->onAudioCancel(document["utteranceId"] | "");
    }
}

bool EdgeLink::isReady() const { return linkState == LinkState::READY; }

LinkState EdgeLink::state() const { return linkState; }

const char* EdgeLink::describeState(LinkState state) {
    switch (state) {
        case LinkState::WIFI_DOWN:    return "SIN_WIFI";
        case LinkState::DISCONNECTED: return "DESCONECTADO";
        case LinkState::CONNECTING:   return "CONECTANDO";
        case LinkState::READY:        return "LISTO";
    }
    return "?";
}

bool EdgeLink::sendBinary(const uint8_t* data, size_t length) {
    if (linkState != LinkState::READY || lock == nullptr) return false;
    if (xSemaphoreTake(lock, SEND_LOCK_WAIT) != pdTRUE) return false;
    const bool sent = socket != nullptr && linkState == LinkState::READY && socket->sendBIN(data, length);
    xSemaphoreGive(lock);
    if (sent) {
        portENTER_CRITICAL(&statsLock);
        counters.bytesSent += length;
        portEXIT_CRITICAL(&statsLock);
    }
    return sent;
}

bool EdgeLink::sendText(const char* json) {
    if (linkState != LinkState::READY || lock == nullptr) return false;
    if (xSemaphoreTake(lock, SEND_LOCK_WAIT) != pdTRUE) return false;
    const bool sent = socket != nullptr && linkState == LinkState::READY && socket->sendTXT(json);
    xSemaphoreGive(lock);
    if (sent) {
        portENTER_CRITICAL(&statsLock);
        counters.bytesSent += strlen(json);
        portEXIT_CRITICAL(&statsLock);
    }
    return sent;
}

bool EdgeLink::sendPlaybackDone(const char* utteranceId) {
    JsonDocument document;
    document["type"] = "playback_done";
    document["utteranceId"] = utteranceId;
    String json;
    serializeJson(document, json);
    return sendText(json.c_str());
}

void EdgeLink::countMicFrames(uint32_t sent, uint32_t droppedNoLink, uint32_t droppedSendFail) {
    portENTER_CRITICAL(&statsLock);
    counters.micFramesSent += sent;
    counters.micFramesDroppedNoLink += droppedNoLink;
    counters.micFramesDroppedSendFail += droppedSendFail;
    portEXIT_CRITICAL(&statsLock);
}

void EdgeLink::addMicQueueDrops(uint32_t drops) {
    portENTER_CRITICAL(&statsLock);
    counters.micQueueDrops += drops;
    portEXIT_CRITICAL(&statsLock);
}

LinkStats EdgeLink::stats() const {
    portENTER_CRITICAL(&statsLock);
    const LinkStats copy = counters;
    portEXIT_CRITICAL(&statsLock);
    return copy;
}

const char* EdgeLink::host() const { return edgeHost; }

uint16_t EdgeLink::port() const { return edgePort; }
