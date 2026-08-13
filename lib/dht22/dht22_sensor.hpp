#pragma once

#include <DHT.h>

#include "model.hpp"

namespace gathra {

class Dht22Sensor {
 public:
  explicit Dht22Sensor(uint8_t pin) : dht_(pin, DHT22) {}
  void begin();
  EnvironmentReading read();
  EnvironmentReading snapshot() const;

 private:
  DHT dht_;
  bool began_ = false;
  bool lastReadOk_ = false;
  bool lastValid_ = false;
  float temperatureC_ = NAN;
  float humidityPercent_ = NAN;
  uint32_t lastAttemptMs_ = 0;
  uint32_t lastSuccessMs_ = 0;
};

}  // namespace gathra
