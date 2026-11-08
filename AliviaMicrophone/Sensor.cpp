#include "Sensor.h"

Sensor::Sensor(int gpioPin, EventHandler* parentHandler) : pin(gpioPin), handler(parentHandler) {}

void Sensor::on(Event event) {
    processEvent(event);
    if (event.identifier != NO_PROPAGATION_IDENTIFIER && handler != nullptr) {
        handler->on(event);
    }
}

void Sensor::setHandler(EventHandler* parentHandler) { handler = parentHandler; }

void Sensor::processEvent(Event& event) { (void)event; }
