#pragma once

#include <stdint.h>

// Immutable production wiring from the archived latest_GATHRA_netlist.xml.
// The older Fritzing FZZ/PNG views are explicitly stale.
// Runtime configuration must never override these values.
namespace gathra::board {
inline constexpr uint8_t kBatteryAdc = 0;
inline constexpr uint8_t kRadioReset = 1;
inline constexpr uint8_t kBuzzer = 2;
inline constexpr uint8_t kRadioDio0 = 3;
inline constexpr uint8_t kRadioSck = 4;
inline constexpr uint8_t kRadioMiso = 5;
inline constexpr uint8_t kRadioMosi = 6;
inline constexpr uint8_t kRadioCs = 7;
inline constexpr uint8_t kRtcSda = 8;
inline constexpr uint8_t kRtcScl = 9;
inline constexpr uint8_t kDhtData = 10;
inline constexpr uint8_t kSonarTrigger = 20;
inline constexpr uint8_t kSonarEcho = 21;

inline constexpr bool kBuzzerActiveHigh = true;
inline constexpr float kBatteryDividerRatio = 3.0F;  // 10 kOhm / 5 kOhm.
}  // namespace gathra::board
