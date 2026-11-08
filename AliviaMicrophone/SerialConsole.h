#ifndef SERIAL_CONSOLE_H
#define SERIAL_CONSOLE_H

/**
 * @file SerialConsole.h
 * @brief Thread-safe Serial output shared by the sketch and the framework tasks.
 *
 * Several FreeRTOS tasks print (loop, NetworkWorker, HardwareWorker). This keeps
 * their lines from mixing and lets the WAV dump mute everyone while it sends binary data.
 */

#include <Arduino.h>

namespace SerialConsole {

void begin(unsigned long baudRate);

/** @brief printf-style line. Dropped silently while the console is muted. */
void printf(const char* format, ...) __attribute__((format(printf, 1, 2)));

/** @brief Mutes every printf() and takes exclusive access for raw binary output. */
void beginExclusive();
void endExclusive();

}  // namespace SerialConsole

#endif // SERIAL_CONSOLE_H
