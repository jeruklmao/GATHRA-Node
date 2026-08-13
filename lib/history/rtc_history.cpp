#include "rtc_history.hpp"

#include <string.h>

namespace gathra {

bool rtcStateValid(const RtcRetainedState& state) {
  return state.magic == build::kRtcMagic &&
         state.schemaVersion == build::kRtcSchemaVersion &&
         state.structureSize == sizeof(RtcRetainedState) &&
         state.historyCount <= build::kHistoryCapacity &&
         state.historyHead < build::kHistoryCapacity &&
         state.filter.historyCount <= build::kMaximumHampelWindow &&
         state.filter.historyHead < build::kMaximumHampelWindow;
}

void initializeRtcState(RtcRetainedState& state, uint32_t bootSessionId) {
  memset(&state, 0, sizeof(state));
  state.magic = build::kRtcMagic;
  state.schemaVersion = build::kRtcSchemaVersion;
  state.structureSize = sizeof(RtcRetainedState);
  state.bootSessionId = bootSessionId;
  state.filter.lastAcceptedMm = kDistanceUnavailable;
  state.filter.candidateMm = kDistanceUnavailable;
  state.filter.lastCandidateMm = kDistanceUnavailable;
  state.filter.lastState = FilterState::kInvalid;
  for (auto& entry : state.history) {
    entry.rawDistanceMm = kDistanceUnavailable;
    entry.acceptedDistanceMm = kDistanceUnavailable;
    entry.temperatureCentiC = kTemperatureUnavailable;
    entry.humidityCentiPercent = kHumidityUnavailable;
  }
}

uint32_t nextSequence(RtcRetainedState& state) { return ++state.sequenceCounter; }

void appendHistory(RtcRetainedState& state, const HistoryEntry& entry) {
  state.history[state.historyHead] = entry;
  state.historyHead = (state.historyHead + 1U) % build::kHistoryCapacity;
  if (state.historyCount < build::kHistoryCapacity) ++state.historyCount;
}

bool historyAt(const RtcRetainedState& state, uint8_t chronologicalIndex,
               HistoryEntry& entry) {
  if (chronologicalIndex >= state.historyCount) return false;
  const uint8_t oldest = state.historyCount < build::kHistoryCapacity ? 0U
                                                                      : state.historyHead;
  entry = state.history[(oldest + chronologicalIndex) % build::kHistoryCapacity];
  return true;
}

void updateNewestHistoryHealth(RtcRetainedState& state, uint16_t healthFlags) {
  if (state.historyCount == 0U) return;
  const uint8_t newest = state.historyHead == 0U ? build::kHistoryCapacity - 1U
                                                 : state.historyHead - 1U;
  state.history[newest].healthFlags = healthFlags;
}

}  // namespace gathra
