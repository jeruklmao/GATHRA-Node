#include "legacy_config_migration.hpp"

#include <stddef.h>
#include <string.h>

namespace gathra {
namespace {

template <typename T>
void checksumFeed(uint32_t& hash, const T& value) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  for (size_t i = 0; i < sizeof(value); ++i) {
    hash ^= bytes[i];
    hash *= 16777619U;
  }
}

}  // namespace

uint32_t legacyConfigV1Checksum(const LegacyNodeConfigV1& config) {
  uint32_t hash = 2166136261U;
#define HASH_LEGACY(name) checksumFeed(hash, config.name)
  HASH_LEGACY(schemaVersion); HASH_LEGACY(nodeId);
  HASH_LEGACY(normalWakeIntervalSec); HASH_LEGACY(changingWakeIntervalSec);
  HASH_LEGACY(sonarBurstCount); HASH_LEGACY(sonarMinimumValid);
  HASH_LEGACY(sonarInterPingMs); HASH_LEGACY(hampelWindow);
  HASH_LEGACY(hampelMultiplier); HASH_LEGACY(hampelAbsoluteFloorMm);
  HASH_LEGACY(suddenChangeThresholdMm); HASH_LEGACY(riseVerificationCount);
  HASH_LEGACY(riseRequiredConfirmations); HASH_LEGACY(riseVerificationIntervalMs);
  HASH_LEGACY(riseToleranceMm); HASH_LEGACY(fallVerificationCount);
  HASH_LEGACY(fallRequiredConfirmations); HASH_LEGACY(fallVerificationIntervalMs);
  HASH_LEGACY(fallToleranceMm); HASH_LEGACY(emaAlpha);
  HASH_LEGACY(referenceDistanceMm); HASH_LEGACY(installationMinimumDistanceMm);
  HASH_LEGACY(installationMaximumDistanceMm); HASH_LEGACY(batteryCalibrationFactor);
  HASH_LEGACY(batteryCalibrationOffsetMv); HASH_LEGACY(batteryLowMv);
  HASH_LEGACY(batteryCriticalMv); HASH_LEGACY(loraFrequencyMhz);
  HASH_LEGACY(loraBandwidthKhz); HASH_LEGACY(loraSpreadingFactor);
  HASH_LEGACY(loraCodingRateDenominator); HASH_LEGACY(loraTxPowerDbm);
  HASH_LEGACY(loraSyncWord); HASH_LEGACY(ackTimeoutMs);
  HASH_LEGACY(ackRetryCount); HASH_LEGACY(maintenanceTimeoutSec);
#undef HASH_LEGACY
  return hash;
}

NodeConfig migrateLegacyConfigV1(const LegacyNodeConfigV1& old,
                                 const char* defaultNodeId) {
  NodeConfig migrated{};
  migrated.setDefaults(defaultNodeId);
  strncpy(migrated.nodeId, old.nodeId, sizeof(migrated.nodeId) - 1U);
  migrated.pollIntervalMinutes =
      pollMinutesFromLegacySeconds(old.normalWakeIntervalSec);
  migrated.sonarBurstCount = old.sonarBurstCount;
  migrated.sonarMinimumValid = old.sonarMinimumValid;
  migrated.sonarInterPingMs = old.sonarInterPingMs;
  migrated.hampelWindow = old.hampelWindow;
  migrated.hampelMultiplier = old.hampelMultiplier;
  migrated.hampelAbsoluteFloorMm = old.hampelAbsoluteFloorMm;
  migrated.suddenChangeThresholdMm = old.suddenChangeThresholdMm;
  migrated.riseVerificationCount = old.riseVerificationCount;
  migrated.riseRequiredConfirmations = old.riseRequiredConfirmations;
  migrated.riseVerificationIntervalMs = old.riseVerificationIntervalMs;
  migrated.riseToleranceMm = old.riseToleranceMm;
  migrated.fallVerificationCount = old.fallVerificationCount;
  migrated.fallRequiredConfirmations = old.fallRequiredConfirmations;
  migrated.fallVerificationIntervalMs = old.fallVerificationIntervalMs;
  migrated.fallToleranceMm = old.fallToleranceMm;
  migrated.emaAlpha = old.emaAlpha;
  migrated.referenceDistanceMm = old.referenceDistanceMm;
  migrated.installationMinimumDistanceMm = old.installationMinimumDistanceMm;
  migrated.installationMaximumDistanceMm = old.installationMaximumDistanceMm;
  migrated.batteryCalibrationFactor = old.batteryCalibrationFactor;
  migrated.batteryCalibrationOffsetMv = old.batteryCalibrationOffsetMv;
  migrated.batteryLowMv = old.batteryLowMv;
  migrated.batteryCriticalMv = old.batteryCriticalMv;
  migrated.loraFrequencyMhz = old.loraFrequencyMhz;
  migrated.loraBandwidthKhz = old.loraBandwidthKhz;
  migrated.loraSpreadingFactor = old.loraSpreadingFactor;
  migrated.loraCodingRateDenominator = old.loraCodingRateDenominator;
  migrated.loraTxPowerDbm = old.loraTxPowerDbm;
  migrated.loraSyncWord = old.loraSyncWord;
  migrated.ackTimeoutMs = old.ackTimeoutMs;
  migrated.ackRetryCount = old.ackRetryCount;
  migrated.maintenanceTimeoutSec =
      old.maintenanceTimeoutSec < 60U
          ? 60U
          : (old.maintenanceTimeoutSec > 300U ? 300U
                                               : old.maintenanceTimeoutSec);
  return migrated;
}

}  // namespace gathra
