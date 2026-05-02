#include <Arduino.h>
#include "Lumox.h"
#include "config.h"

void setup() {
    Serial.begin(SERIAL_BAUD);
    Lumox::instance().begin();
}

void loop() {
    Lumox::instance().loop();
}
