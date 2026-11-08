#ifndef SENSOR_H
#define SENSOR_H

/**
 * @file Sensor.h
 * @brief Base class of every sensor.
 *
 * on(event) runs processEvent(), which may rewrite the event (e.g. turn a MEASURE request
 * into DATA_READ) or set its identifier to -1 to say "nothing to report". The resulting
 * event is then propagated to the parent handler.
 */

#include "EventHandler.h"

class Sensor : public EventHandler {
public:
    static constexpr int MEASURE_DATA_REQUESTED_EVENT_IDENTIFIER = 1;
    static constexpr int DATA_READ_EVENT_IDENTIFIER = 2;

    explicit Sensor(int gpioPin, EventHandler* parentHandler = nullptr);

    void on(Event event) override;
    void setHandler(EventHandler* parentHandler);

protected:
    /** @brief Hook for subclasses. Default: leave the event unchanged. */
    virtual void processEvent(Event& event);

    int pin;
    EventHandler* handler;

private:
    static constexpr int NO_PROPAGATION_IDENTIFIER = -1;
};

#endif // SENSOR_H
