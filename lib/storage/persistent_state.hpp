#pragma once

#include <stddef.h>
#include <stdint.h>

#include "filter.hpp"
#include "protocol.hpp"

#ifdef ARDUINO
#include <Preferences.h>
#endif

namespace gathra {

inline constexpr uint32_t kPersistentStateMagic = 0x47545632U;  // GTV2
inline constexpr uint16_t kPersistentStateSchema = 2;

enum class ExpectedRebootMode : uint8_t {
  kNone = 0,
  kMaintenance = 1,
  kOta = 2,
};

enum class CommandPhase : uint8_t {
  kNone = 0,
  kReceived = 1,
  kCompleted = 2,
};

enum class ShutdownReason : uint8_t {
  kNone = 0,
  kPollSuccess = 1,
  kPollError = 2,
  kMaintenanceExpired = 3,
  kMaintenanceExit = 4,
  kRecovery = 5,
  kPowerProgrammingFault = 6,
};

struct StoredCommand {
  uint32_t commandId = 0;
  protocol::CommandType commandType = protocol::CommandType::kNone;
  CommandPhase phase = CommandPhase::kNone;
  protocol::CommandResultCode result = protocol::CommandResultCode::kNone;
  uint8_t requestedPollIntervalMinutes = 0;
  uint8_t effectivePollIntervalMinutes = 0;
  uint32_t requestedMaintenanceUnix = 0;
  uint32_t effectiveMaintenanceUnix = 0;
};

struct PowerEvent {
  uint32_t eventCounter = 0;
  protocol::BootReason bootReason = protocol::BootReason::kUnknown;
  uint8_t pcfStatusAtBoot = 0;
  uint32_t rtcUnixAtBoot = 0;
  ShutdownReason shutdownReason = ShutdownReason::kNone;
  uint8_t timerMinutes = 0;
  uint32_t scheduledMaintenanceUnix = 0;
  bool flagClearAttempted = false;
  bool flagClearSucceeded = false;
  uint32_t commandId = 0;
};

struct PersistentState {
  uint32_t magic = kPersistentStateMagic;
  uint16_t schemaVersion = kPersistentStateSchema;
  uint16_t structureSize = 0;
  uint32_t persistentSessionId = 0;
  uint32_t nextSequence = 1;
  FilterMemory filter{};
  protocol::ScheduleState scheduleState = protocol::ScheduleState::kNone;
  uint32_t scheduledMaintenanceUnix = 0;
  uint32_t lastRtcSyncUnix = 0;
  uint32_t lastGatewayUtc = 0;
  StoredCommand command{};
  ExpectedRebootMode expectedReboot = ExpectedRebootMode::kNone;
  bool maintenanceActive = false;
  uint32_t maintenanceDeadlineUnix = 0;
  uint32_t powerEventCounter = 0;
  uint8_t powerEventHead = 0;
  uint8_t powerEventCount = 0;
  PowerEvent powerEvents[8]{};
  uint32_t checksum = 0;
};

class StateBackend {
 public:
  virtual ~StateBackend() = default;
  virtual bool load(PersistentState& state) = 0;
  virtual bool save(const PersistentState& state) = 0;
};

#ifdef ARDUINO
class NvsStateBackend final : public StateBackend {
 public:
  bool load(PersistentState& state) override;
  bool save(const PersistentState& state) override;
};
#endif

class PersistentStateManager {
 public:
  bool begin(StateBackend& backend, uint32_t generatedSessionId,
             bool& initializedFresh);
  bool commit();
  bool allocateSequence(uint32_t& sequence);
  bool appendPowerEvent(const PowerEvent& event);
  // A battery-powered flag clear normally removes power before the confirming
  // NVS write can finish.  The next genuine cold-power boot is independent
  // evidence that the previous release transaction completed.
  bool reconcileLastFlagClearAfterColdBoot(bool coldBootEvidence,
                                           bool& reconciled);
  PersistentState& state() { return state_; }
  const PersistentState& state() const { return state_; }
  const char* lastError() const { return lastError_; }

  static void initialize(PersistentState& state, uint32_t sessionId);
  static bool valid(const PersistentState& state);
  static uint32_t checksum(const PersistentState& state);

 private:
  StateBackend* backend_ = nullptr;
  PersistentState state_{};
  const char* lastError_ = "not initialized";
};

const char* expectedRebootName(ExpectedRebootMode mode);
const char* shutdownReasonName(ShutdownReason reason);

}  // namespace gathra
