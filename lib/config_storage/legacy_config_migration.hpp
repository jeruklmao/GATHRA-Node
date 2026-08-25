#pragma once

#include <stdint.h>

#include "node_config.hpp"

namespace gathra {

// Exact persisted Protocol/config v1 layout. It is retained only as the
// read-only one-time migration source for schema v2.
struct LegacyNodeConfigV1 {
  uint16_t schemaVersion = 1;
  char nodeId[build::kNodeIdCapacity]{};
  uint32_t normalWakeIntervalSec = 60;
  uint32_t changingWakeIntervalSec = 12;
  uint8_t sonarBurstCount = 7;
  uint8_t sonarMinimumValid = 5;
  uint16_t sonarInterPingMs = 60;
  uint8_t hampelWindow = 7;
  float hampelMultiplier = 3.0F;
  uint16_t hampelAbsoluteFloorMm = 50;
  uint16_t suddenChangeThresholdMm = 100;
  uint8_t riseVerificationCount = 5;
  uint8_t riseRequiredConfirmations = 4;
  uint16_t riseVerificationIntervalMs = 2000;
  uint16_t riseToleranceMm = 75;
  uint8_t fallVerificationCount = 5;
  uint8_t fallRequiredConfirmations = 4;
  uint16_t fallVerificationIntervalMs = 5000;
  uint16_t fallToleranceMm = 100;
  float emaAlpha = 0.25F;
  uint32_t referenceDistanceMm = 0;
  uint32_t installationMinimumDistanceMm = 0;
  uint32_t installationMaximumDistanceMm = 0;
  float batteryCalibrationFactor = 1.0F;
  int16_t batteryCalibrationOffsetMv = 0;
  uint16_t batteryLowMv = 3500;
  uint16_t batteryCriticalMv = 3300;
  float loraFrequencyMhz = 433.0F;
  float loraBandwidthKhz = 125.0F;
  uint8_t loraSpreadingFactor = 10;
  uint8_t loraCodingRateDenominator = 6;
  int8_t loraTxPowerDbm = 17;
  uint8_t loraSyncWord = 0x12;
  uint16_t ackTimeoutMs = 1800;
  uint8_t ackRetryCount = 2;
  uint16_t maintenanceTimeoutSec = 300;
};

uint32_t legacyConfigV1Checksum(const LegacyNodeConfigV1& config);
NodeConfig migrateLegacyConfigV1(const LegacyNodeConfigV1& old,
                                 const char* defaultNodeId);

}  // namespace gathra
