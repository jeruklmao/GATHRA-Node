#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gathra::build {
inline constexpr uint32_t kSerialBaud = 115200;
inline constexpr char kMaintenancePassword[] = "sman35jakarta";
inline constexpr char kStoragePartition[] = "nvs_v2";
inline constexpr uint32_t kMaintenanceMaximumSec = 300;
inline constexpr uint32_t kRtcSyncDriftThresholdSec = 5;
inline constexpr uint32_t kManualLatchTimeoutMs = 1000;
inline constexpr uint32_t kEnvironmentFreshMs = 10000;
inline constexpr uint32_t kDhtMinimumReadMs = 2200;
inline constexpr uint32_t kSonarEchoTimeoutUs = 35000;
inline constexpr uint32_t kMinimumEchoUs = 100;
inline constexpr uint32_t kMaximumEchoUs = 30000;
inline constexpr uint8_t kMaximumSonarSamples = 15;
inline constexpr uint8_t kMaximumHampelWindow = 15;
inline constexpr uint16_t kHistoryCapacity = 512;
inline constexpr uint8_t kLogCapacity = 64;
inline constexpr size_t kNodeIdCapacity = 25;  // 24 characters plus NUL.
inline constexpr size_t kRadioPacketCapacity = 96;
}  // namespace gathra::build
