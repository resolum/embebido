/**
 * Alivia Voice - ESP32 #1: microfono INMP441 (I2S) sobre el framework ModestIoT.
 *
 * Etapa actual: solo el microfono, sin conexion al edge. El objetivo es comprobar
 * que el audio llega limpio a PCM 16 kHz / 16 bit / mono, que es lo que consumira
 * el edge de voz por WebSocket (/audio-in) mas adelante.
 *
 * Arquitectura (ModestIoT):
 *   Inmp441Microphone (Sensor)  -> tarea de captura I2S propia, estadisticas y VAD
 *   MicrophoneDevice  (Device)  -> scheduler por timer y telemetria
 *
 * Requisitos:
 *   - Placa: ESP32 Dev Module (ESP32 clasico). Core "esp32" de Espressif 3.x.
 *   - Librerias: ModestIoT y sus dependencias (ArduinoJson, DHT sensor library,
 *     ESP32Servo, HX711, LiquidCrystal I2C, PubSubClient).
 *   - Monitor serie a 115200 baudios, fin de linea "Ninguno" o "Nueva linea".
 *
 * Comandos por el monitor serie (una letra + Enter):
 *   h  ayuda           s  estado            v  vumetro en vivo on/off
 *   +  ganancia +3 dB  -  ganancia -3 dB    j  JSON de telemetria on/off
 *   r  prueba de 3 s analizada en el ESP32 (pico, nivel, recortes, veredicto)
 *   R  graba 3 s y envia un WAV binario (usar tools/grabar_wav.py, no el monitor)
 */

#include <ModestIoT.h>
#include <esp_heap_caps.h>

#include "MicrophoneConfig.h"
#include "Inmp441Microphone.h"
#include "MicrophoneDevice.h"
#include "SerialConsole.h"

// ---------------------------------------------------------------------------
// Objetos del framework (el I2S se inicia en setup(), no en los constructores)
// ---------------------------------------------------------------------------
Inmp441Microphone microphone(MICROPHONE_SCK_PIN, MICROPHONE_WS_PIN, MICROPHONE_SD_PIN, AUDIO_SAMPLE_RATE);
MicrophoneDevice microphoneDevice(DEVICE_ID, microphone, SCHEDULER_TICK_MS);

bool liveMeterEnabled = false;
bool microphoneReady = false;
unsigned long lastMeterTimestamp = 0;

// ---------------------------------------------------------------------------
// Utilidades de grabacion
// ---------------------------------------------------------------------------

/** Reserva el buffer mas grande posible hasta `seconds` (usa PSRAM si la placa la tiene). */
int16_t* allocateRecordingBuffer(uint32_t seconds, size_t& sampleCount) {
    sampleCount = static_cast<size_t>(seconds) * AUDIO_SAMPLE_RATE;
    auto* buffer = static_cast<int16_t*>(heap_caps_malloc(sampleCount * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer != nullptr) return buffer;

    // Sin PSRAM: el bloque libre mas grande de la RAM interna suele rondar 100 KB (~3 s).
    const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const size_t reserve = 16 * 1024;  // dejar aire para Serial, JSON y las tareas
    size_t fitting = largestBlock > reserve ? (largestBlock - reserve) / sizeof(int16_t) : 0;
    fitting -= fitting % Inmp441Microphone::FRAME_SAMPLES;
    if (fitting < sampleCount) sampleCount = fitting;
    if (sampleCount < AUDIO_SAMPLE_RATE / 2) return nullptr;  // menos de 0.5 s no sirve
    return static_cast<int16_t*>(heap_caps_malloc(sampleCount * sizeof(int16_t), MALLOC_CAP_8BIT));
}

/** Llena `buffer` con audio nuevo del microfono. Devuelve las muestras grabadas. */
size_t recordInto(int16_t* buffer, size_t sampleCount) {
    if (!microphone.startRecording(buffer, sampleCount)) return 0;
    const unsigned long timeout = (sampleCount * 1000UL) / AUDIO_SAMPLE_RATE + 2000;
    const unsigned long start = millis();
    while (microphone.isRecording() && millis() - start < timeout) {
        delay(20);
    }
    const size_t recorded = microphone.getRecordedSampleCount();
    microphone.cancelRecording();
    return recorded;
}

float toDbfs(float amplitude) {
    return amplitude <= 0.0f ? -120.0f : 20.0f * log10f(amplitude / 32768.0f);
}

void runTestRecording() {
    size_t samples = 0;
    int16_t* buffer = allocateRecordingBuffer(TEST_RECORDING_SECONDS, samples);
    if (buffer == nullptr) {
        SerialConsole::printf("[Prueba] Sin memoria para grabar.\n");
        return;
    }
    SerialConsole::printf("[Prueba] Grabando %.1f s... HABLA AHORA (di \"alivia, prende la luz\").\n",
                          static_cast<float>(samples) / AUDIO_SAMPLE_RATE);
    const size_t recorded = recordInto(buffer, samples);

    int peak = 0;
    uint32_t clipped = 0;
    double sumOfSquares = 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < recorded; ++i) {
        const int value = buffer[i];
        const int magnitude = abs(value);
        if (magnitude > peak) peak = magnitude;
        if (magnitude >= 32767) ++clipped;
        sumOfSquares += static_cast<double>(value) * value;
        sum += value;
    }
    free(buffer);

    if (recorded == 0) {
        SerialConsole::printf("[Prueba] No llego audio. Revisa el estado en las lineas [Mic].\n");
        return;
    }
    const float rms = static_cast<float>(sqrt(sumOfSquares / recorded));
    const float mean = static_cast<float>(sum / recorded);
    const float peakDbfs = toDbfs(static_cast<float>(peak));

    SerialConsole::printf("[Prueba] muestras=%u  pico=%d (%.1f dBFS)  nivel=%.1f dBFS  recortes=%lu  DC=%.1f\n",
                          static_cast<unsigned>(recorded), peak, peakDbfs, toDbfs(rms),
                          static_cast<unsigned long>(clipped), mean);

    if (peak == 0) {
        SerialConsole::printf("[Prueba] Veredicto: SOLO CEROS. Revisa SD, VDD (3.3V), GND y L/R.\n");
    } else if (clipped > recorded / 1000) {
        SerialConsole::printf("[Prueba] Veredicto: RECORTA. Baja la ganancia con '-' (ahora %.0f dB).\n",
                              microphone.getGainInDecibels());
    } else if (peakDbfs < -45.0f) {
        SerialConsole::printf("[Prueba] Veredicto: MUY BAJO. Habla mas cerca o sube la ganancia con '+'.\n");
    } else if (peakDbfs < -25.0f) {
        SerialConsole::printf("[Prueba] Veredicto: BAJO pero usable. Para el edge conviene un pico de -25 a -6 dBFS.\n");
    } else {
        SerialConsole::printf("[Prueba] Veredicto: OK. Buen nivel para Vosk/Whisper.\n");
    }
}

void writeLittleEndian(uint8_t* destination, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) destination[i] = static_cast<uint8_t>(value >> (8 * i));
}

/** Graba y envia un WAV por Serial entre marcadores, para tools/grabar_wav.py. */
void sendWavRecording() {
    size_t samples = 0;
    int16_t* buffer = allocateRecordingBuffer(WAV_RECORDING_SECONDS, samples);
    if (buffer == nullptr) {
        SerialConsole::printf("@@ALIVIA_WAV_ERROR sin_memoria\n");
        return;
    }
    SerialConsole::printf("@@ALIVIA_WAV_RECORDING seconds=%.1f\n", static_cast<float>(samples) / AUDIO_SAMPLE_RATE);
    const size_t recorded = recordInto(buffer, samples);
    const uint32_t dataBytes = recorded * sizeof(int16_t);

    uint8_t header[44];
    memcpy(header, "RIFF", 4);
    writeLittleEndian(header + 4, 36 + dataBytes, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    writeLittleEndian(header + 16, 16, 4);                    // tamano del bloque fmt
    writeLittleEndian(header + 20, 1, 2);                     // PCM
    writeLittleEndian(header + 22, 1, 2);                     // mono
    writeLittleEndian(header + 24, AUDIO_SAMPLE_RATE, 4);
    writeLittleEndian(header + 28, AUDIO_SAMPLE_RATE * 2, 4); // bytes por segundo
    writeLittleEndian(header + 32, 2, 2);                     // bytes por muestra
    writeLittleEndian(header + 34, 16, 2);                    // bits por muestra
    memcpy(header + 36, "data", 4);
    writeLittleEndian(header + 40, dataBytes, 4);

    SerialConsole::beginExclusive();
    Serial.printf("\n@@ALIVIA_WAV_BEGIN size=%lu rate=%lu\n",
                  static_cast<unsigned long>(sizeof(header) + dataBytes), static_cast<unsigned long>(AUDIO_SAMPLE_RATE));
    Serial.write(header, sizeof(header));
    const auto* bytes = reinterpret_cast<const uint8_t*>(buffer);
    for (uint32_t offset = 0; offset < dataBytes; offset += 512) {
        Serial.write(bytes + offset, min<uint32_t>(512, dataBytes - offset));
    }
    Serial.print("\n@@ALIVIA_WAV_END\n");
    SerialConsole::endExclusive();
    free(buffer);
}

// ---------------------------------------------------------------------------
// Consola
// ---------------------------------------------------------------------------
void printHelp() {
    SerialConsole::printf(
        "\nComandos:\n"
        "  h  ayuda                 s  estado\n"
        "  v  vumetro en vivo       j  JSON de telemetria\n"
        "  +  ganancia +3 dB        -  ganancia -3 dB\n"
        "  r  prueba de %lu s (habla durante la prueba)\n"
        "  R  enviar WAV al PC (solo con tools/grabar_wav.py)\n\n",
        static_cast<unsigned long>(TEST_RECORDING_SECONDS));
}

void printStatus() {
    const AudioLevelReport report = microphone.getLastReport();
    SerialConsole::printf(
        "[Estado] pines SCK=%d WS=%d SD=%d | %lu Hz | canal %s | ganancia %.0f dB | estado %s\n"
        "[Estado] %s\n"
        "[Estado] heap libre %u B, bloque mayor %u B, PSRAM %u B\n",
        MICROPHONE_SCK_PIN, MICROPHONE_WS_PIN, MICROPHONE_SD_PIN, static_cast<unsigned long>(AUDIO_SAMPLE_RATE),
        microphone.getActiveSlot() == 0 ? "L" : microphone.getActiveSlot() == 1 ? "R" : "?",
        microphone.getGainInDecibels(), Inmp441Microphone::describeStatus(report.status),
        Inmp441Microphone::describeStatusHint(report.status),
        static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
        static_cast<unsigned>(ESP.getFreePsram()));
}

void changeGain(float deltaInDecibels) {
    microphone.setGainInDecibels(microphone.getGainInDecibels() + deltaInDecibels);
    SerialConsole::printf("[Mic] Ganancia: %.0f dB\n", microphone.getGainInDecibels());
}

void handleSerialCommand(char command) {
    switch (command) {
        case 'h': case '?': printHelp(); break;
        case 's': printStatus(); break;
        case 'v':
            liveMeterEnabled = !liveMeterEnabled;
            microphoneDevice.setReportsVisible(!liveMeterEnabled);
            SerialConsole::printf("[Mic] Vumetro en vivo: %s\n", liveMeterEnabled ? "ON" : "OFF");
            break;
        case 'j':
            microphoneDevice.setJsonOutputEnabled(!microphoneDevice.isJsonOutputEnabled());
            SerialConsole::printf("[Mic] JSON: %s\n", microphoneDevice.isJsonOutputEnabled() ? "ON" : "OFF");
            break;
        case '+': changeGain(3.0f); break;
        case '-': changeGain(-3.0f); break;
        case 'r': runTestRecording(); break;
        case 'R': sendWavRecording(); break;
        case '\r': case '\n': case ' ': break;
        default: SerialConsole::printf("[Mic] Comando desconocido '%c'. Escribe h.\n", command); break;
    }
}

void printLiveMeter() {
    const float level = microphone.getInstantLevelInDbfs();
    char bar[41];
    int filled = static_cast<int>((level + 90.0f) / 90.0f * 40.0f + 0.5f);
    filled = constrain(filled, 0, 40);
    for (int i = 0; i < 40; ++i) bar[i] = i < filled ? '#' : '.';
    bar[40] = '\0';
    SerialConsole::printf("[VU] %s %6.1f dBFS %s\n", bar, level, microphone.isVoiceActive() ? "VOZ" : "");
}

// ---------------------------------------------------------------------------
void setup() {
    SerialConsole::begin(SERIAL_BAUD_RATE);
    delay(300);
    SerialConsole::printf("\n=== Alivia Voice | ESP32 #1 microfono INMP441 | ModestIoT ===\n");

    microphone.setGainInDecibels(MICROPHONE_GAIN_DB);
    microphone.setVoiceThresholdInDecibels(VOICE_THRESHOLD_DB);
    microphone.setVoiceHangoverInMilliseconds(VOICE_HANGOVER_MS);
    microphone.setReportIntervalInMilliseconds(LEVEL_REPORT_INTERVAL_MS);

    if (!microphone.begin(MICROPHONE_CHANNEL)) {
        SerialConsole::printf("[Boot] ERROR: no se pudo iniciar I2S. Revisa los pines y que el core ESP32 sea 3.x.\n");
        return;
    }
    microphoneReady = true;
    microphoneDevice.begin();

    SerialConsole::printf("[Boot] Microfono listo: %lu Hz, 16 bit, mono. SCK=%d WS=%d SD=%d, ganancia %.0f dB\n",
                          static_cast<unsigned long>(AUDIO_SAMPLE_RATE), MICROPHONE_SCK_PIN, MICROPHONE_WS_PIN,
                          MICROPHONE_SD_PIN, microphone.getGainInDecibels());
    printHelp();
}

void loop() {
    if (!microphoneReady) {
        delay(1000);
        return;
    }

    while (Serial.available() > 0) {
        handleSerialCommand(static_cast<char>(Serial.read()));
    }

    if (liveMeterEnabled && millis() - lastMeterTimestamp >= SCHEDULER_TICK_MS) {
        lastMeterTimestamp = millis();
        printLiveMeter();
    }
    delay(10);
}
