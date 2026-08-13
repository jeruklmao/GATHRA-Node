#pragma once

#include <stdint.h>

// Immutable production wiring from GATHRA.fzz/GATHRA_netlist.xml.
// Runtime configuration must never override these values.
namespace gathra::board {
inline constexpr uint8_t kBatteryAdc = 0;
inline constexpr uint8_t kRadioReset = 1;
inline constexpr uint8_t kButton = 2;
inline constexpr uint8_t kRadioDio0 = 3;
inline constexpr uint8_t kRadioSck = 4;
inline constexpr uint8_t kRadioMiso = 5;
inline constexpr uint8_t kRadioMosi = 6;
inline constexpr uint8_t kRadioCs = 7;
inline constexpr uint8_t kDhtData = 10;
inline constexpr uint8_t kSonarTrigger = 20;
inline constexpr uint8_t kSonarEcho = 21;

inline constexpr bool kButtonActiveLow = true;
inline constexpr float kBatteryDividerRatio = 3.0F;  // 10 kOhm / 5 kOhm.
}  // namespace gathra::board
