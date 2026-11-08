#ifndef TELEMETRY_PACKAGE_H
#define TELEMETRY_PACKAGE_H

/**
 * @file TelemetryPackage.h
 * @brief Base class for telemetry payloads queued by a Device and serialized to JSON.
 */

#include <ArduinoJson.h>

class TelemetryPackage {
public:
    virtual ~TelemetryPackage() = default;

    /** @brief Writes this package's fields into `document`. */
    virtual void serialize(JsonDocument& document) const = 0;
};

#endif // TELEMETRY_PACKAGE_H
