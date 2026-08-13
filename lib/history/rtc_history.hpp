#pragma once

#include <stddef.h>
#include <stdint.h>

#include "build_config.hpp"
#include "filter.hpp"
#include "model.hpp"

namespace gathra {

struct HistoryEntry {
  uint32_t sequence = 0;
  uint32_t rawDistanceMm = kDistanceUnavailable;
  uint32_t acceptedDistanceMm = kDistanceUnavailable;
  uint16_t madMm = 0;
  uint16_t batteryMv = 0;
  int16_t temperatureCentiC = kTemperatureUnavailable;
  uint16_t humidityCentiPercent = kHumidityUnavailable;
  uint16_t healthFlags = 0;
  uint8_t filterState = static_cast<uint8_t>(FilterState::kInvalid);
  uint8_t validSamples = 0;
  uint8_t totalSamples = 0;
  uint8_t reserved = 0;
};

struct RtcRetainedState {
  uint32_t magic = 0;
  uint16_t schemaVersion = 0;
  uint16_t structureSize = 0;
  uint32_t bootSessionId = 0;
  uint32_t sequenceCounter = 0;
  FilterMemory filter{};
  HistoryEntry history[build::kHistoryCapacity]{};
  uint8_t historyCount = 0;
  uint8_t historyHead = 0;
  uint16_t reserved = 0;
};

static_assert(sizeof(RtcRetainedState) < 4096U,
              "RTC retained state must remain a conservative fraction of RTC memory");

bool rtcStateValid(const RtcRetainedState& state);
void initializeRtcState(RtcRetainedState& state, uint32_t bootSessionId);
uint32_t nextSequence(RtcRetainedState& state);
void appendHistory(RtcRetainedState& state, const HistoryEntry& entry);
bool historyAt(const RtcRetainedState& state, uint8_t chronologicalIndex,
               HistoryEntry& entry);
void updateNewestHistoryHealth(RtcRetainedState& state, uint16_t healthFlags);

}  // namespace gathra
