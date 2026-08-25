#include "command_processor.hpp"

#include <string.h>

namespace gathra {

protocol::CommandResultPacket CommandProcessor::makePacket(
    const StoredCommand& command, const char* nodeId,
    uint32_t persistentSessionId) {
  protocol::CommandResultPacket packet{};
  if (nodeId != nullptr) strncpy(packet.nodeId, nodeId, sizeof(packet.nodeId) - 1U);
  packet.persistentSessionId = persistentSessionId;
  packet.commandId = command.commandId;
  packet.commandType = command.commandType;
  packet.resultCode = command.result;
  packet.effectivePollIntervalMinutes = command.effectivePollIntervalMinutes;
  packet.scheduledMaintenanceUnix = command.effectiveMaintenanceUnix;
  return packet;
}

CommandApplyResult CommandProcessor::handle(const protocol::AckCommandPacket& ack,
                                            const char* nodeId,
                                            uint32_t persistentSessionId) {
  CommandApplyResult outcome{};
  if (ack.commandType == protocol::CommandType::kNone || ack.commandId == 0U) {
    return outcome;
  }

  const StoredCommand previous = environment_.storedCommand();
  if (previous.commandId == ack.commandId &&
      previous.commandType == ack.commandType &&
      previous.phase == CommandPhase::kCompleted) {
    outcome.sendResult = true;
    outcome.duplicate = true;
    // A duplicate result is re-sent, but the maintenance side effect is not
    // started again after an earlier maintenance lifetime has completed.
    outcome.enterMaintenance = false;
    outcome.packet = makePacket(previous, nodeId, persistentSessionId);
    return outcome;
  }

  // Reusing an ID for a different command is invalid. The prior durable
  // command is retained and no side effect is attempted.
  if (previous.commandId == ack.commandId && previous.commandId != 0U &&
      previous.commandType != ack.commandType) {
    StoredCommand rejected{};
    rejected.commandId = ack.commandId;
    rejected.commandType = ack.commandType;
    rejected.phase = CommandPhase::kCompleted;
    rejected.result = protocol::CommandResultCode::kInvalidArgument;
    rejected.effectivePollIntervalMinutes = environment_.effectivePollIntervalMinutes();
    rejected.effectiveMaintenanceUnix = environment_.effectiveMaintenanceUnix();
    outcome.sendResult = true;
    outcome.packet = makePacket(rejected, nodeId, persistentSessionId);
    return outcome;
  }

  StoredCommand current{};
  current.commandId = ack.commandId;
  current.commandType = ack.commandType;
  current.phase = CommandPhase::kReceived;
  current.requestedPollIntervalMinutes = ack.pollIntervalMinutes;
  current.requestedMaintenanceUnix = ack.scheduledMaintenanceUnix;
  if (!(previous.commandId == ack.commandId &&
        previous.commandType == ack.commandType &&
        previous.phase == CommandPhase::kReceived)) {
    if (!environment_.persistReceipt(current)) {
      current.phase = CommandPhase::kCompleted;
      current.result = protocol::CommandResultCode::kStorageError;
      current.effectivePollIntervalMinutes = environment_.effectivePollIntervalMinutes();
      current.effectiveMaintenanceUnix = environment_.effectiveMaintenanceUnix();
      outcome.sendResult = true;
      outcome.packet = makePacket(current, nodeId, persistentSessionId);
      return outcome;
    }
  } else {
    current = previous;  // Resume an interrupted, already-durable receipt.
  }

  switch (current.commandType) {
    case protocol::CommandType::kEnterMaintenanceNow:
      current.result = environment_.applyEnterMaintenance();
      break;
    case protocol::CommandType::kSetPollIntervalMinutes:
      current.result = current.requestedPollIntervalMinutes == 0U
                           ? protocol::CommandResultCode::kInvalidArgument
                           : environment_.applyPollInterval(
                                 current.requestedPollIntervalMinutes);
      break;
    case protocol::CommandType::kScheduleMaintenanceAt:
      current.result = current.requestedMaintenanceUnix == 0U
                           ? protocol::CommandResultCode::kInvalidArgument
                           : environment_.applyMaintenanceSchedule(
                                 current.requestedMaintenanceUnix);
      break;
    case protocol::CommandType::kNone:
      current.result = protocol::CommandResultCode::kInvalidArgument;
      break;
  }
  current.effectivePollIntervalMinutes = environment_.effectivePollIntervalMinutes();
  current.effectiveMaintenanceUnix = environment_.effectiveMaintenanceUnix();
  current.phase = CommandPhase::kCompleted;
  if (!environment_.persistResult(current)) {
    current.phase = CommandPhase::kReceived;
    current.result = protocol::CommandResultCode::kStorageError;
  }
  outcome.sendResult = true;
  outcome.enterMaintenance =
      current.commandType == protocol::CommandType::kEnterMaintenanceNow &&
      current.result == protocol::CommandResultCode::kApplied;
  outcome.packet = makePacket(current, nodeId, persistentSessionId);
  return outcome;
}

}  // namespace gathra
