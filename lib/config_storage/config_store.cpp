#include "config_store.hpp"

#include <Preferences.h>
#include <string.h>

namespace gathra {
namespace {

uint32_t checksum(const NodeConfig& config) {
  uint32_t hash = 2166136261U;
  const auto feed = [&hash](const void* value, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(value);
    for (size_t i = 0; i < length; ++i) {
      hash ^= bytes[i];
      hash *= 16777619U;
    }
  };
#define HASH_FIELD(name) feed(&config.name, sizeof(config.name))
  HASH_FIELD(schemaVersion); HASH_FIELD(nodeId);
  HASH_FIELD(normalWakeIntervalSec); HASH_FIELD(changingWakeIntervalSec);
  HASH_FIELD(sonarBurstCount); HASH_FIELD(sonarMinimumValid); HASH_FIELD(sonarInterPingMs);
  HASH_FIELD(hampelWindow); HASH_FIELD(hampelMultiplier); HASH_FIELD(hampelAbsoluteFloorMm);
  HASH_FIELD(suddenChangeThresholdMm); HASH_FIELD(riseVerificationCount);
  HASH_FIELD(riseRequiredConfirmations); HASH_FIELD(riseVerificationIntervalMs);
  HASH_FIELD(riseToleranceMm); HASH_FIELD(fallVerificationCount);
  HASH_FIELD(fallRequiredConfirmations); HASH_FIELD(fallVerificationIntervalMs);
  HASH_FIELD(fallToleranceMm); HASH_FIELD(emaAlpha); HASH_FIELD(referenceDistanceMm);
  HASH_FIELD(installationMinimumDistanceMm); HASH_FIELD(installationMaximumDistanceMm);
  HASH_FIELD(batteryCalibrationFactor); HASH_FIELD(batteryCalibrationOffsetMv);
  HASH_FIELD(batteryLowMv); HASH_FIELD(batteryCriticalMv); HASH_FIELD(loraFrequencyMhz);
  HASH_FIELD(loraBandwidthKhz); HASH_FIELD(loraSpreadingFactor);
  HASH_FIELD(loraCodingRateDenominator); HASH_FIELD(loraTxPowerDbm);
  HASH_FIELD(loraSyncWord); HASH_FIELD(ackTimeoutMs); HASH_FIELD(ackRetryCount);
  HASH_FIELD(maintenanceTimeoutSec);
#undef HASH_FIELD
  return hash;
}

}  // namespace

bool ConfigStore::load(NodeConfig& config, const char* defaultNodeId) {
  config.setDefaults(defaultNodeId);
  Preferences prefs;
  // Read-write mode creates the namespace on a genuinely fresh device, which
  // lets an empty store be distinguished from an NVS open failure.
  if (!prefs.begin("gathra", false)) {
    lastError_ = "NVS namespace open failed; defaults loaded";
    return false;
  }
  const size_t storedLength = prefs.getBytesLength("config");
  if (storedLength == 0U) {
    prefs.end();
    lastError_ = "no stored configuration; defaults loaded";
    return true;
  }
  if (storedLength != sizeof(NodeConfig)) {
    prefs.end();
    lastError_ = "stored configuration size/schema mismatch; defaults loaded";
    return false;
  }
  NodeConfig candidate{};
  const size_t read = prefs.getBytes("config", &candidate, sizeof(candidate));
  const uint32_t storedChecksum = prefs.getUInt("checksum", 0U);
  prefs.end();
  if (read != sizeof(candidate) || storedChecksum != checksum(candidate)) {
    lastError_ = "stored configuration checksum mismatch; defaults loaded";
    return false;
  }
  const ConfigValidationResult validation = validateConfig(candidate);
  if (!validation) {
    lastError_ = validation.message;
    return false;
  }
  config = candidate;
  lastError_ = "configuration loaded";
  return true;
}

bool ConfigStore::save(const NodeConfig& config) {
  const ConfigValidationResult validation = validateConfig(config);
  if (!validation) {
    lastError_ = validation.message;
    return false;
  }
  Preferences prefs;
  if (!prefs.begin("gathra", false)) {
    lastError_ = "NVS namespace open failed";
    return false;
  }
  const size_t written = prefs.putBytes("config", &config, sizeof(config));
  const size_t checksumWritten = prefs.putUInt("checksum", checksum(config));
  prefs.end();
  if (written != sizeof(config) || checksumWritten != sizeof(uint32_t)) {
    lastError_ = "NVS write failed";
    return false;
  }
  lastError_ = "configuration saved";
  return true;
}

}  // namespace gathra
