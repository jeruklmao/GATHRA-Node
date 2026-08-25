#include "persistent_state.hpp"

#include <stddef.h>
#include <string.h>

#include "build_config.hpp"

namespace gathra {

uint32_t PersistentStateManager::checksum(const PersistentState& state) {
  uint32_t hash = 2166136261U;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&state);
  for (size_t i = 0; i < offsetof(PersistentState, checksum); ++i) {
    hash ^= bytes[i];
    hash *= 16777619U;
  }
  return hash;
}

void PersistentStateManager::initialize(PersistentState& state, uint32_t sessionId) {
  memset(&state, 0, sizeof(state));
  state.magic = kPersistentStateMagic;
  state.schemaVersion = kPersistentStateSchema;
  state.structureSize = sizeof(PersistentState);
  state.persistentSessionId = sessionId == 0U ? 1U : sessionId;
  state.nextSequence = 1U;
  state.filter.lastAcceptedMm = kDistanceUnavailable;
  state.filter.candidateMm = kDistanceUnavailable;
  state.filter.lastCandidateMm = kDistanceUnavailable;
  state.filter.lastState = FilterState::kInvalid;
  state.command.result = protocol::CommandResultCode::kNone;
  state.checksum = checksum(state);
}

bool PersistentStateManager::valid(const PersistentState& state) {
  if (state.magic != kPersistentStateMagic ||
      state.schemaVersion != kPersistentStateSchema ||
      state.structureSize != sizeof(PersistentState) ||
      state.persistentSessionId == 0U || state.nextSequence == 0U ||
      state.filter.historyCount > build::kMaximumHampelWindow ||
      state.filter.historyHead >= build::kMaximumHampelWindow ||
      state.powerEventHead >= 8U || state.powerEventCount > 8U ||
      static_cast<uint8_t>(state.scheduleState) >
          static_cast<uint8_t>(protocol::ScheduleState::kFailed) ||
      static_cast<uint8_t>(state.expectedReboot) >
          static_cast<uint8_t>(ExpectedRebootMode::kOta) ||
      static_cast<uint8_t>(state.command.phase) >
          static_cast<uint8_t>(CommandPhase::kCompleted)) return false;
  return state.checksum == checksum(state);
}

bool PersistentStateManager::begin(StateBackend& backend, uint32_t generatedSessionId,
                                   bool& initializedFresh) {
  backend_ = &backend;
  initializedFresh = false;
  PersistentState loaded{};
  if (backend.load(loaded) && valid(loaded)) {
    state_ = loaded;
    lastError_ = "persistent state loaded";
    return true;
  }
  initializedFresh = true;
  initialize(state_, generatedSessionId);
  if (!backend.save(state_)) {
    lastError_ = "persistent state unavailable and initialization write failed";
    return false;
  }
  lastError_ = "persistent state initialized (new protocol session)";
  return true;
}

bool PersistentStateManager::commit() {
  if (backend_ == nullptr) {
    lastError_ = "persistent state backend unavailable";
    return false;
  }
  state_.checksum = checksum(state_);
  if (!backend_->save(state_)) {
    lastError_ = "persistent state write failed";
    return false;
  }
  lastError_ = "persistent state committed";
  return true;
}

bool PersistentStateManager::allocateSequence(uint32_t& sequence) {
  sequence = 0U;
  if (backend_ == nullptr || state_.nextSequence == 0U ||
      state_.nextSequence == UINT32_MAX) {
    lastError_ = "sequence space exhausted or state backend unavailable";
    return false;
  }
  PersistentState candidate = state_;
  sequence = candidate.nextSequence;
  ++candidate.nextSequence;
  candidate.checksum = checksum(candidate);
  if (!backend_->save(candidate)) {
    sequence = 0U;
    lastError_ = "next sequence could not be persisted before TX";
    return false;
  }
  state_ = candidate;
  lastError_ = "sequence allocated and next sequence persisted";
  return true;
}

bool PersistentStateManager::appendPowerEvent(const PowerEvent& supplied) {
  PowerEvent event = supplied;
  event.eventCounter = ++state_.powerEventCounter;
  state_.powerEvents[state_.powerEventHead] = event;
  state_.powerEventHead = static_cast<uint8_t>((state_.powerEventHead + 1U) % 8U);
  if (state_.powerEventCount < 8U) ++state_.powerEventCount;
  return commit();
}

bool PersistentStateManager::reconcileLastFlagClearAfterColdBoot(
    bool coldBootEvidence, bool& reconciled) {
  reconciled = false;
  if (!coldBootEvidence || state_.powerEventCount == 0U) return true;
  if (backend_ == nullptr) {
    lastError_ = "persistent state backend unavailable";
    return false;
  }
  const uint8_t index = state_.powerEventHead == 0U
                            ? 7U
                            : static_cast<uint8_t>(state_.powerEventHead - 1U);
  const PowerEvent& previous = state_.powerEvents[index];
  if (!previous.flagClearAttempted || previous.flagClearSucceeded) return true;

  PersistentState candidate = state_;
  candidate.powerEvents[index].flagClearSucceeded = true;
  candidate.checksum = checksum(candidate);
  if (!backend_->save(candidate)) {
    lastError_ = "cold-boot flag-clear reconciliation write failed";
    return false;
  }
  state_ = candidate;
  reconciled = true;
  lastError_ = "previous flag clear confirmed by subsequent cold-power boot";
  return true;
}

#ifdef ARDUINO
bool NvsStateBackend::load(PersistentState& state) {
  Preferences preferences;
  if (!preferences.begin("gathra-state", true, build::kStoragePartition)) return false;
  const size_t length = preferences.getBytesLength("state");
  const size_t read = length == sizeof(state)
                          ? preferences.getBytes("state", &state, sizeof(state))
                          : 0U;
  preferences.end();
  return read == sizeof(state);
}

bool NvsStateBackend::save(const PersistentState& state) {
  Preferences preferences;
  if (!preferences.begin("gathra-state", false, build::kStoragePartition)) return false;
  const size_t written = preferences.putBytes("state", &state, sizeof(state));
  preferences.end();
  return written == sizeof(state);
}
#endif

const char* expectedRebootName(ExpectedRebootMode mode) {
  switch (mode) {
    case ExpectedRebootMode::kNone: return "NONE";
    case ExpectedRebootMode::kMaintenance: return "MAINTENANCE_REBOOT";
    case ExpectedRebootMode::kOta: return "OTA_REBOOT";
  }
  return "UNKNOWN";
}

const char* shutdownReasonName(ShutdownReason reason) {
  switch (reason) {
    case ShutdownReason::kNone: return "NONE";
    case ShutdownReason::kPollSuccess: return "POLL_SUCCESS";
    case ShutdownReason::kPollError: return "POLL_ERROR";
    case ShutdownReason::kMaintenanceExpired: return "MAINTENANCE_EXPIRED";
    case ShutdownReason::kMaintenanceExit: return "MAINTENANCE_EXIT";
    case ShutdownReason::kRecovery: return "RECOVERY";
    case ShutdownReason::kPowerProgrammingFault: return "POWER_PROGRAMMING_FAULT";
  }
  return "UNKNOWN";
}

}  // namespace gathra
