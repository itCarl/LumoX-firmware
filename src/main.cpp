#include <Arduino.h>
#include "Lumox.h"
#include "config.h"

void setup() {
    LOG_BEGIN(SERIAL_BAUD);
    Lumox::instance().begin();
}

void loop() {
    Lumox::instance().loop();
}
