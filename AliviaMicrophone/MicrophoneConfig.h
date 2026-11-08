#ifndef MICROPHONE_CONFIG_H
#define MICROPHONE_CONFIG_H

/**
 * @file MicrophoneConfig.h
 * @brief Wiring and tuning for ESP32 #1 (INMP441 microphone).
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
 * No LED and no display in this build: the only peripheral is the microphone.
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

// --- Serial recordings ---
static constexpr uint32_t TEST_RECORDING_SECONDS = 3;            // 'r': analyse 3 s on the ESP32
static constexpr uint32_t WAV_RECORDING_SECONDS = 3;             // 'R': send a WAV to the PC (tools/grabar_wav.py)

#endif // MICROPHONE_CONFIG_H
