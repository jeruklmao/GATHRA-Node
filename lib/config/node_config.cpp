#include "node_config.hpp"

#include <ctype.h>
#include <math.h>
#include <string.h>

namespace gathra {
namespace {

ConfigValidationResult fail(ConfigValidationCode code, const char* message) {
  return {code, message};
}

bool validBandwidth(float value) {
  constexpr float kValues[] = {7.8F, 10.4F, 15.6F, 20.8F, 31.25F,
                               41.7F, 62.5F, 125.0F, 250.0F};
  for (float candidate : kValues) {
    if (fabsf(value - candidate) < 0.06F) return true;
  }
  return false;
}

bool inDistanceRange(uint32_t mm) { return mm >= 20U && mm <= 30000U; }

}  // namespace

void NodeConfig::setDefaults(const char* defaultNodeId) {
  *this = NodeConfig{};
  if (defaultNodeId != nullptr) {
    strncpy(nodeId, defaultNodeId, sizeof(nodeId) - 1U);
    nodeId[sizeof(nodeId) - 1U] = '\0';
  }
}

bool nodeIdValid(const char* nodeId) {
  if (nodeId == nullptr) return false;
  const size_t length = strnlen(nodeId, build::kNodeIdCapacity);
  if (length == 0U || length >= build::kNodeIdCapacity) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(nodeId[i]);
    if (!(isalnum(c) || c == '-' || c == '_')) return false;
  }
  return true;
}

ConfigValidationResult validateConfig(const NodeConfig& c) {
  if (c.schemaVersion != kConfigSchemaVersion) {
    return fail(ConfigValidationCode::kSchema, "unsupported configuration schema");
  }
  if (!nodeIdValid(c.nodeId)) {
    return fail(ConfigValidationCode::kNodeId, "nodeId must be 1-24 ASCII letters, digits, '-' or '_'");
  }
  if (c.normalWakeIntervalSec < 15U || c.normalWakeIntervalSec > 86400U ||
      c.changingWakeIntervalSec < 2U || c.changingWakeIntervalSec > 3600U ||
      c.changingWakeIntervalSec >= c.normalWakeIntervalSec) {
    return fail(ConfigValidationCode::kWakeInterval, "wake interval is outside the safe range");
  }
  if (c.sonarBurstCount < 3U || c.sonarBurstCount > build::kMaximumSonarSamples ||
      c.sonarMinimumValid == 0U || c.sonarMinimumValid > c.sonarBurstCount ||
      c.sonarInterPingMs < 50U || c.sonarInterPingMs > 1000U) {
    return fail(ConfigValidationCode::kSonar, "invalid sonar burst/minimum/inter-ping setting");
  }
  if (c.hampelWindow < 3U || c.hampelWindow > build::kMaximumHampelWindow ||
      (c.hampelWindow % 2U) == 0U || !isfinite(c.hampelMultiplier) ||
      c.hampelMultiplier < 1.0F || c.hampelMultiplier > 10.0F ||
      c.hampelAbsoluteFloorMm < 1U || c.hampelAbsoluteFloorMm > 5000U ||
      c.suddenChangeThresholdMm < 1U || c.suddenChangeThresholdMm > 10000U) {
    return fail(ConfigValidationCode::kHampel, "invalid Hampel or sudden-change setting");
  }
  const bool riseInvalid =
      c.riseVerificationCount < 2U || c.riseVerificationCount > build::kMaximumSonarSamples ||
      c.riseRequiredConfirmations == 0U ||
      c.riseRequiredConfirmations > c.riseVerificationCount ||
      c.riseVerificationIntervalMs < 100U || c.riseVerificationIntervalMs > 60000U ||
      c.riseToleranceMm < 1U || c.riseToleranceMm > 5000U;
  const bool fallInvalid =
      c.fallVerificationCount < 2U || c.fallVerificationCount > build::kMaximumSonarSamples ||
      c.fallRequiredConfirmations == 0U ||
      c.fallRequiredConfirmations > c.fallVerificationCount ||
      c.fallVerificationIntervalMs < 100U || c.fallVerificationIntervalMs > 60000U ||
      c.fallToleranceMm < 1U || c.fallToleranceMm > 5000U;
  if (riseInvalid || fallInvalid ||
      c.riseVerificationIntervalMs > c.fallVerificationIntervalMs) {
    return fail(ConfigValidationCode::kVerification, "invalid rise/fall verification policy");
  }
  if (!isfinite(c.emaAlpha) || c.emaAlpha <= 0.0F || c.emaAlpha > 1.0F) {
    return fail(ConfigValidationCode::kEma, "emaAlpha must be greater than 0 and at most 1");
  }
  if ((c.referenceDistanceMm != 0U && !inDistanceRange(c.referenceDistanceMm)) ||
      ((c.installationMinimumDistanceMm == 0U) !=
       (c.installationMaximumDistanceMm == 0U)) ||
      (c.installationMinimumDistanceMm != 0U &&
       (!inDistanceRange(c.installationMinimumDistanceMm) ||
        !inDistanceRange(c.installationMaximumDistanceMm) ||
        c.installationMinimumDistanceMm >= c.installationMaximumDistanceMm))) {
    return fail(ConfigValidationCode::kCalibration,
                "reference must be disabled or 20-30000 mm; installation limits must be a valid pair");
  }
  if (!isfinite(c.batteryCalibrationFactor) || c.batteryCalibrationFactor < 0.5F ||
      c.batteryCalibrationFactor > 1.5F || c.batteryCalibrationOffsetMv < -1000 ||
      c.batteryCalibrationOffsetMv > 1000 || c.batteryCriticalMv < 2000U ||
      c.batteryLowMv > 6000U || c.batteryCriticalMv >= c.batteryLowMv) {
    return fail(ConfigValidationCode::kBattery, "invalid battery calibration or thresholds");
  }
  if (!isfinite(c.loraFrequencyMhz) || c.loraFrequencyMhz < 410.0F ||
      c.loraFrequencyMhz > 525.0F || !validBandwidth(c.loraBandwidthKhz) ||
      c.loraSpreadingFactor < 7U || c.loraSpreadingFactor > 12U ||
      c.loraCodingRateDenominator < 5U || c.loraCodingRateDenominator > 8U ||
      c.loraTxPowerDbm < 2 || c.loraTxPowerDbm > 20) {
    return fail(ConfigValidationCode::kRadio, "invalid SX1278 radio setting");
  }
  if (c.ackTimeoutMs < 500U || c.ackTimeoutMs > 10000U || c.ackRetryCount > 2U) {
    return fail(ConfigValidationCode::kAck, "ACK timeout must be 500-10000 ms and retries 0-2");
  }
  if (c.maintenanceTimeoutSec < 60U || c.maintenanceTimeoutSec > 3600U) {
    return fail(ConfigValidationCode::kMaintenance, "maintenance timeout must be 60-3600 seconds");
  }
  return {};
}

bool radioConfigEqual(const NodeConfig& a, const NodeConfig& b) {
  return fabsf(a.loraFrequencyMhz - b.loraFrequencyMhz) < 0.001F &&
         fabsf(a.loraBandwidthKhz - b.loraBandwidthKhz) < 0.01F &&
         a.loraSpreadingFactor == b.loraSpreadingFactor &&
         a.loraCodingRateDenominator == b.loraCodingRateDenominator &&
         a.loraTxPowerDbm == b.loraTxPowerDbm && a.loraSyncWord == b.loraSyncWord;
}

}  // namespace gathra
