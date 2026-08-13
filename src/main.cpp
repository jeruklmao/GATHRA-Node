#include <Arduino.h>

#include "node_app.hpp"

gathra::NodeApp app;

void setup() { app.begin(); }

void loop() { app.run(); }
