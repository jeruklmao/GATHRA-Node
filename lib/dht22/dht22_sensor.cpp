#include "dht22_sensor.hpp"

#include <Arduino.h>
#include <math.h>

#include "build_config.hpp"
#include "logger.hpp"

namespace gathra {
namespace {

float speedOfSound(float temperatureC, float humidityPercent) {
  return 331.3F + 0.606F * temperatureC + 0.0124F * humidityPercent;
}

}  // namespace

void Dht22Sensor::begin() {
  dht_.begin(55U);
  began_ = true;
}

EnvironmentReading Dht22Sensor::snapshot() const {
  EnvironmentReading result{};
  result.sensorReadOk = lastReadOk_;
  if (!lastValid_) return result;
  const uint32_t age = millis() - lastSuccessMs_;
  result.temperatureC = temperatureC_;
  result.humidityPercent = humidityPercent_;
  result.ageMs = age;
  result.stale = age > build::kEnvironmentFreshMs;
  result.valid = !result.stale;
  result.speedOfSoundMps = result.valid ? speedOfSound(temperatureC_, humidityPercent_)
                                        : 343.0F;
  return result;
}

EnvironmentReading Dht22Sensor::read() {
  if (!began_) begin();
  const uint32_t now = millis();
  if (lastAttemptMs_ != 0U && now - lastAttemptMs_ < build::kDhtMinimumReadMs) {
    return snapshot();
  }
  lastAttemptMs_ = now;
  const bool readOk = dht_.read(false);
  const float humidity = readOk ? dht_.readHumidity(false) : NAN;
  const float temperature = readOk ? dht_.readTemperature(false, false) : NAN;
  if (readOk && isfinite(humidity) && isfinite(temperature) && humidity >= 0.0F &&
      humidity <= 100.0F && temperature >= -40.0F && temperature <= 80.0F) {
    humidityPercent_ = humidity;
    temperatureC_ = temperature;
    lastSuccessMs_ = millis();
    lastValid_ = true;
    lastReadOk_ = true;
    GTH_LOGI("DHT", "temperature=%.1f C humidity=%.1f %%", temperature, humidity);
  } else {
    lastReadOk_ = false;
    GTH_LOGW("DHT", "read failed; acoustic fallback will be used");
  }
  return snapshot();
}

}  // namespace gathra
