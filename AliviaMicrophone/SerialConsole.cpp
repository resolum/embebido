#include "SerialConsole.h"

#include <stdarg.h>

namespace {
SemaphoreHandle_t consoleMutex = nullptr;
volatile bool muted = false;
}  // namespace

namespace SerialConsole {

void begin(unsigned long baudRate) {
    Serial.begin(baudRate);
    if (consoleMutex == nullptr) {
        consoleMutex = xSemaphoreCreateMutex();
    }
}

void printf(const char* format, ...) {
    if (muted || consoleMutex == nullptr) return;

    char line[384];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);

    if (xSemaphoreTake(consoleMutex, portMAX_DELAY) == pdTRUE) {
        if (!muted) Serial.print(line);
        xSemaphoreGive(consoleMutex);
    }
}

void beginExclusive() {
    muted = true;
    if (consoleMutex != nullptr) xSemaphoreTake(consoleMutex, portMAX_DELAY);
    Serial.flush();
}

void endExclusive() {
    Serial.flush();
    if (consoleMutex != nullptr) xSemaphoreGive(consoleMutex);
    muted = false;
}

}  // namespace SerialConsole
