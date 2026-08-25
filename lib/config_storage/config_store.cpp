#include "config_store.hpp"

#ifdef ARDUINO
#include <Preferences.h>
#include <string.h>

#include "legacy_config_migration.hpp"

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
  HASH_FIELD(pollIntervalMinutes);
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
  Preferences current;
  if (!current.begin("gathra", false, build::kStoragePartition)) {
    lastError_ = "v2 NVS partition/namespace open failed; defaults loaded";
    return false;
  }
  const size_t currentLength = current.getBytesLength("config");
  if (currentLength != 0U) {
    if (currentLength != sizeof(NodeConfig)) {
      current.end();
      lastError_ = "v2 configuration size mismatch; defaults loaded";
      return false;
    }
    NodeConfig candidate{};
    const size_t read = current.getBytes("config", &candidate, sizeof(candidate));
    const uint32_t storedChecksum = current.getUInt("checksum", 0U);
    current.end();
    if (read != sizeof(candidate) || storedChecksum != checksum(candidate)) {
      lastError_ = "v2 configuration checksum mismatch; defaults loaded";
      return false;
    }
    const ConfigValidationResult validation = validateConfig(candidate);
    if (!validation) {
      lastError_ = validation.message;
      return false;
    }
    config = candidate;
    lastError_ = "v2 configuration loaded";
    return true;
  }
  current.end();

  // The original 20 KiB partition is read only as a migration source. All
  // successful paths below persist into nvs_v2 and never rewrite the v1 copy.
  Preferences legacySource;
  if (!legacySource.begin("gathra", true)) {
    if (!save(config)) {
      lastError_ = "no legacy namespace; initial v2 defaults persistence failed";
      return false;
    }
    lastError_ = "no legacy namespace; fresh defaults initialized in v2 NVS";
    return true;
  }
  const size_t legacyLength = legacySource.getBytesLength("config");
  if (legacyLength == 0U) {
    legacySource.end();
    if (!save(config)) {
      lastError_ = "fresh defaults valid but initial v2 persistence failed";
      return false;
    }
    lastError_ = "fresh defaults initialized in v2 NVS";
    return true;
  }
  if (legacyLength == sizeof(LegacyNodeConfigV1)) {
    LegacyNodeConfigV1 legacy{};
    const size_t read = legacySource.getBytes("config", &legacy, sizeof(legacy));
    const uint32_t storedChecksum = legacySource.getUInt("checksum", 0U);
    legacySource.end();
    if (read != sizeof(legacy) || legacy.schemaVersion != 1U ||
        storedChecksum != legacyConfigV1Checksum(legacy)) {
      lastError_ = "v1 configuration checksum/schema invalid; all fields reset to defaults";
      return false;
    }
    NodeConfig migrated = migrateLegacyConfigV1(legacy, defaultNodeId);
    const ConfigValidationResult validation = validateConfig(migrated);
    if (!validation) {
      lastError_ = "v1 configuration migration validation failed; all fields reset to defaults";
      return false;
    }
    config = migrated;
    if (!save(config)) {
      lastError_ = "v1 configuration migrated in RAM but v2 persistence failed";
      return false;
    }
    lastError_ = "configuration migrated v1->v2; legacy seconds rounded up/clamped to 1-255 minutes";
    return true;
  }
  // This also relocates a v2 config written by an intermediate development
  // image that still used the original partition.
  if (legacyLength != sizeof(NodeConfig)) {
    legacySource.end();
    lastError_ = "legacy configuration size/schema unknown; defaults loaded";
    return false;
  }
  NodeConfig candidate{};
  const size_t read = legacySource.getBytes("config", &candidate, sizeof(candidate));
  const uint32_t storedChecksum = legacySource.getUInt("checksum", 0U);
  legacySource.end();
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
  if (!save(config)) {
    lastError_ = "valid configuration found in legacy NVS but relocation to v2 failed";
    return false;
  }
  lastError_ = "v2 configuration relocated from legacy NVS partition";
  return true;
}

bool ConfigStore::save(const NodeConfig& config) {
  const ConfigValidationResult validation = validateConfig(config);
  if (!validation) {
    lastError_ = validation.message;
    return false;
  }
  Preferences prefs;
  if (!prefs.begin("gathra", false, build::kStoragePartition)) {
    lastError_ = "v2 NVS namespace open failed";
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

#endif  // ARDUINO
