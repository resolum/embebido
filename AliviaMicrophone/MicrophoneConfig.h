#ifndef MICROPHONE_CONFIG_H
#define MICROPHONE_CONFIG_H

/**
 * @file MicrophoneConfig.h
 * @brief Wiring and tuning for ESP32 #1 (INMP441 microphone + MAX98357A speaker amplifier).
 *
 * Wiring (ESP32 DevKit / WROOM):
 *   INMP441 VDD -> 3V3        (never 5V)
 *   INMP441 GND -> GND
 *   INMP441 L/R -> GND        (left slot; the driver also works with L/R to 3V3)
 *   INMP441 SCK -> GPIO 27
 *   INMP441 WS  -> GPIO 14
 *   INMP441 SD  -> GPIO 2     (strapping pin: fine as a data input; if uploading ever fails,
 *                              unplug SD while flashing)
 *
 * Speaker (MAX98357A, second I2S port, TX only):
 *   MAX98357A BCLK -> GPIO 26
 *   MAX98357A LRC  -> GPIO 25   (word select)
 *   MAX98357A DIN  -> GPIO 22
 *   MAX98357A VIN  -> 5V (VIN pin of the DevKit; see README for the power notes)
 *   MAX98357A GND  -> GND
 *   MAX98357A SD   -> not connected (both I2S slots carry the same sample, so any channel mode is full level)
 *   MAX98357A GAIN -> not connected (9 dB)
 *
 * No LED and no display in this build: the peripherals are the microphone and the speaker.
 */

#include "Inmp441Microphone.h"

// --- Identity ---
static constexpr const char* DEVICE_ID = "alivia-mic-01";

// --- Serial ---
static constexpr unsigned long SERIAL_BAUD_RATE = 115200;

// --- INMP441 pins ---
static constexpr int MICROPHONE_SCK_PIN = 27;
static constexpr int MICROPHONE_WS_PIN  = 14;
static constexpr int MICROPHONE_SD_PIN  = 2;
static constexpr Inmp441Microphone::Channel MICROPHONE_CHANNEL = Inmp441Microphone::Channel::AUTO;

// --- Audio (must match the voice edge: PCM 16 kHz, 16 bit, mono) ---
static constexpr uint32_t AUDIO_SAMPLE_RATE = 16000;

// Digital gain. Talking normally at ~50 cm the "pico" should land between -25 and -10 dBFS.
// Too low -> raise with '+'. Clipping ("clip" > 0 while talking) -> lower with '-'.
static constexpr float MICROPHONE_GAIN_DB = 24.0f;

// --- Voice activity detector ---
static constexpr float VOICE_THRESHOLD_DB = 12.0f;               // dB above the noise floor
static constexpr unsigned long VOICE_HANGOVER_MS = 600;          // silence needed to close a segment

// --- ModestIoT scheduler ---
static constexpr unsigned long SCHEDULER_TICK_MS = 100;          // MEASURE request period
static constexpr unsigned long LEVEL_REPORT_INTERVAL_MS = 1000;  // one level line per second

// --- Link to the voice edge (credentials live in Secrets.h) ---
static constexpr uint16_t EDGE_PORT = 8765;
static constexpr const char* EDGE_PATH = "/ws";
static constexpr unsigned long LINK_CONNECT_TIMEOUT_MS = 5000;   // TCP + WebSocket handshake + "ready"
static constexpr unsigned long LINK_BACKOFF_MIN_MS = 1000;       // reconnect backoff: 1 s ... 10 s
static constexpr unsigned long LINK_BACKOFF_MAX_MS = 10000;
static constexpr unsigned long LINK_PING_INTERVAL_MS = 10000;    // WebSocket ping every ~10 s
static constexpr unsigned long LINK_PONG_TIMEOUT_MS = 5000;
static constexpr uint8_t LINK_MAX_MISSED_PONGS = 3;              // 3 pings without answer = link down
static constexpr UBaseType_t LINK_TASK_PRIORITY = 3;
static constexpr uint32_t LINK_TASK_STACK_BYTES = 8192;

// --- Microphone uplink (core 0, lower priority than the link and the playback) ---
static constexpr size_t MIC_QUEUE_FRAMES = 25;                   // 25 x 640 B = 16 KB, ~500 ms
static constexpr size_t STREAM_FRAMES_PER_MESSAGE = 2;           // 1..4 frames of 20 ms per WebSocket message
static constexpr UBaseType_t STREAM_TASK_PRIORITY = 2;
static constexpr uint32_t STREAM_TASK_STACK_BYTES = 3072;
static constexpr bool SEND_AUDIO_LEVEL_TELEMETRY = true;         // voice_activity is always sent

// --- Speaker (MAX98357A) ---
static constexpr int SPEAKER_BCLK_PIN = 26;
static constexpr int SPEAKER_LRC_PIN  = 25;
static constexpr int SPEAKER_DIN_PIN  = 22;
static constexpr uint32_t PLAYBACK_SAMPLE_RATE = 16000;          // announced in hello; the edge resamples to it
static constexpr uint8_t SPEAKER_VOLUME_PERCENT = 60;            // ceiling for the output level (0..100)
static constexpr uint32_t SPEAKER_BUFFER_MS = 500;               // jitter buffer capacity (16 KB at 16 kHz)
static constexpr uint32_t SPEAKER_PREFILL_MS = 200;              // audio buffered before sound starts
static constexpr UBaseType_t PLAYBACK_TASK_PRIORITY = 4;
static constexpr uint32_t PLAYBACK_TASK_STACK_BYTES = 4096;
static constexpr uint32_t TEST_TONE_HZ = 440;                    // 't' command
static constexpr uint32_t TEST_TONE_MS = 1000;

// --- Serial recordings ---
static constexpr uint32_t TEST_RECORDING_SECONDS = 3;            // 'r': analyse 3 s on the ESP32
static constexpr uint32_t WAV_RECORDING_SECONDS = 3;             // 'R': send a WAV to the PC (tools/grabar_wav.py)

#endif // MICROPHONE_CONFIG_H
