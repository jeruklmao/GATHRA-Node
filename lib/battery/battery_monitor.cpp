#include "battery_monitor.hpp"

#include <Arduino.h>
#include <math.h>

#include "board_pins.hpp"
#include "logger.hpp"

namespace gathra {

void BatteryMonitor::begin() {
  analogReadResolution(12);
  analogSetPinAttenuation(board::kBatteryAdc, ADC_11db);
}

BatteryReading BatteryMonitor::read(const NodeConfig& config) {
  constexpr uint8_t kSamples = 9;
  uint32_t samples[kSamples]{};
  uint32_t rawSamples[kSamples]{};
  for (uint8_t i = 0; i < kSamples; ++i) {
    rawSamples[i] = analogRead(board::kBatteryAdc);
    samples[i] = analogReadMilliVolts(board::kBatteryAdc);
    delay(3);
  }
  for (uint8_t i = 1; i < kSamples; ++i) {
    const uint32_t value = samples[i];
    uint8_t j = i;
    while (j > 0U && samples[j - 1U] > value) {
      samples[j] = samples[j - 1U];
      --j;
    }
    samples[j] = value;
  }
  for (uint8_t i = 1; i < kSamples; ++i) {
    const uint32_t value = rawSamples[i];
    uint8_t j = i;
    while (j > 0U && rawSamples[j - 1U] > value) {
      rawSamples[j] = rawSamples[j - 1U];
      --j;
    }
    rawSamples[j] = value;
  }
  uint32_t sum = 0;
  for (uint8_t i = 2; i < kSamples - 2U; ++i) sum += samples[i];
  const uint32_t adcMv = sum / (kSamples - 4U);
  const float corrected =
      static_cast<float>(adcMv) * board::kBatteryDividerRatio *
          config.batteryCalibrationFactor +
      static_cast<float>(config.batteryCalibrationOffsetMv);
  BatteryReading result{};
  result.adcValid = adcMv >= 10U && adcMv <= 3100U && isfinite(corrected) && corrected > 0.0F;
  result.adcMillivolts = static_cast<uint16_t>(adcMv > UINT16_MAX ? UINT16_MAX : adcMv);
  result.adcRaw = static_cast<uint16_t>(rawSamples[kSamples / 2U]);
  const long rounded = lroundf(corrected);
  result.batteryMillivolts = static_cast<uint16_t>(
      rounded < 0 ? 0 : (rounded > UINT16_MAX ? UINT16_MAX : rounded));
  result.low = result.adcValid && result.batteryMillivolts <= config.batteryLowMv;
  result.critical = result.adcValid && result.batteryMillivolts <= config.batteryCriticalMv;
  GTH_LOGI("BATTERY", "raw=%u adc=%u mV battery=%u mV valid=%s",
           result.adcRaw, result.adcMillivolts, result.batteryMillivolts,
           result.adcValid ? "yes" : "no");
  return result;
}

}  // namespace gathra
