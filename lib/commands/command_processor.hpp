#pragma once

#include "persistent_state.hpp"

namespace gathra {

struct CommandApplyResult {
  bool sendResult = false;
  bool duplicate = false;
  bool enterMaintenance = false;
  protocol::CommandResultPacket packet{};
};

class CommandEnvironment {
 public:
  virtual ~CommandEnvironment() = default;
  virtual const StoredCommand& storedCommand() const = 0;
  virtual bool persistReceipt(const StoredCommand& command) = 0;
  virtual protocol::CommandResultCode applyEnterMaintenance() = 0;
  virtual protocol::CommandResultCode applyPollInterval(uint8_t minutes) = 0;
  virtual protocol::CommandResultCode applyMaintenanceSchedule(uint32_t targetUnix) = 0;
  virtual bool persistResult(const StoredCommand& command) = 0;
  virtual uint8_t effectivePollIntervalMinutes() const = 0;
  virtual uint32_t effectiveMaintenanceUnix() const = 0;
};

class CommandProcessor {
 public:
  explicit CommandProcessor(CommandEnvironment& environment) : environment_(environment) {}
  CommandApplyResult handle(const protocol::AckCommandPacket& ack,
                            const char* nodeId, uint32_t persistentSessionId);

 private:
  static protocol::CommandResultPacket makePacket(const StoredCommand& command,
                                                   const char* nodeId,
                                                   uint32_t persistentSessionId);
  CommandEnvironment& environment_;
};

}  // namespace gathra
