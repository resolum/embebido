#ifndef EVENT_HANDLER_H
#define EVENT_HANDLER_H

/**
 * @file EventHandler.h
 * @brief Event value and handler interface of the local ModestIoT-style scheduler layer.
 */

/** @brief Something that happened in a sensor. `identifier` may be rewritten by Sensor::processEvent(). */
struct Event {
    int identifier;  ///< Event type.
    int source;      ///< GPIO pin of the sensor that produced it (-1 if none).

    constexpr explicit Event(int eventIdentifier, int sourcePin = -1)
        : identifier(eventIdentifier), source(sourcePin) {}
};

class EventHandler {
public:
    virtual void on(Event event) = 0;
    virtual ~EventHandler() = default;
};

#endif // EVENT_HANDLER_H
