#include "sensor_suite.hpp"

#include <Arduino.h>
#include <math.h>

#include "board_pins.hpp"
#include "filter.hpp"
#include "logger.hpp"

namespace gathra {

SensorSuite::SensorSuite()
    : dht_(board::kDhtData),
      sonar_(static_cast<gpio_num_t>(board::kSonarTrigger),
             static_cast<gpio_num_t>(board::kSonarEcho)) {}

bool SensorSuite::begin() {
  dht_.begin();
  battery_.begin();
  sonarReady_ = sonar_.begin();
  if (!sonarReady_) GTH_LOGE("SONAR", "GPIO/ISR initialization failed");
  return sonarReady_;
}

EnvironmentReading SensorSuite::readEnvironment() { return dht_.read(); }

BatteryReading SensorSuite::readBattery(const NodeConfig& config) {
  return battery_.read(config);
}

SonarBurst SensorSuite::readSonarBurst(const NodeConfig& config,
                                      const EnvironmentReading& environment) {
  SonarBurst burst{};
  burst.totalSamples = config.sonarBurstCount;
  uint32_t validDistances[build::kMaximumSonarSamples]{};
  uint32_t validEchoes[build::kMaximumSonarSamples]{};
  if (!sonarReady_) return burst;

  const float acousticSpeed = environment.valid ? environment.speedOfSoundMps : 344.8F;
  for (uint8_t i = 0; i < config.sonarBurstCount; ++i) {
    EchoSample sample = sonar_.measure(build::kSonarEchoTimeoutUs);
    if (sample.status == EchoStatus::kOk) {
      const float distance = static_cast<float>(sample.pulseUs) * acousticSpeed / 2000.0F;
      if (isfinite(distance) && distance >= 1.0F && distance <= UINT16_MAX) {
        sample.distanceMm = static_cast<uint16_t>(lroundf(distance));
        const bool limitsEnabled = config.installationMinimumDistanceMm != 0U;
        const bool withinLimits =
            !limitsEnabled ||
            (sample.distanceMm >= config.installationMinimumDistanceMm &&
             sample.distanceMm <= config.installationMaximumDistanceMm);
        if (withinLimits) {
          validDistances[burst.validSamples] = sample.distanceMm;
          validEchoes[burst.validSamples] = sample.pulseUs;
          ++burst.validSamples;
        } else {
          sample.status = EchoStatus::kOutsideInstallationRange;
        }
      }
    }
    burst.samples[i] = sample;
    GTH_LOGD("SONAR", "ping=%u status=%s edges=%u/%u echo=%lu us distance=%u mm",
             static_cast<unsigned>(i + 1U), Srf05Driver::statusName(sample.status),
             sample.risingEdges, sample.fallingEdges,
             static_cast<unsigned long>(sample.pulseUs), sample.distanceMm);
    if (i + 1U < config.sonarBurstCount) delay(config.sonarInterPingMs);
  }

  if (burst.validSamples < config.sonarMinimumValid) {
    GTH_LOGW("SONAR", "burst invalid valid=%u/%u", burst.validSamples, burst.totalSamples);
    return burst;
  }
  const RobustStats distanceStats = robustStats(validDistances, burst.validSamples);
  const RobustStats echoStats = robustStats(validEchoes, burst.validSamples);
  burst.valid = distanceStats.valid && echoStats.valid;
  burst.medianDistanceMm = distanceStats.median;
  burst.madMm = static_cast<uint16_t>(distanceStats.mad > UINT16_MAX
                                          ? UINT16_MAX
                                          : distanceStats.mad);
  burst.medianEchoUs = echoStats.median;
  GTH_LOGI("SONAR", "burst valid=%u/%u median=%lu mm MAD=%u mm env=%s",
           burst.validSamples, burst.totalSamples,
           static_cast<unsigned long>(burst.medianDistanceMm), burst.madMm,
           environment.valid ? "compensated" : "reference");
  return burst;
}

}  // namespace gathra
