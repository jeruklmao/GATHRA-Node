#pragma once

#include "pcf8563.hpp"
#include "persistent_state.hpp"

namespace gathra {

struct BootClassification {
  protocol::BootReason reason = protocol::BootReason::kUnknown;
  bool enterMaintenance = false;
  bool timerFlagAtBoot = false;
  bool alarmFlagAtBoot = false;
};

class PowerManager {
 public:
  explicit PowerManager(rtc::Pcf8563& rtc) : rtc_(rtc) {}

  static BootClassification classify(const rtc::Status& status,
                                     ExpectedRebootMode expectedReboot,
                                     rtc::TimeState timeState,
                                     uint32_t rtcUnix,
                                     protocol::ScheduleState scheduleState,
                                     uint32_t scheduledTargetUnix);

  bool establishManualLatch(uint32_t timeoutMs);
  bool programNextWake(uint8_t pollIntervalMinutes,
                       protocol::ScheduleState scheduleState,
                       uint32_t scheduledTargetUnix);
  bool verifyNextWake(uint8_t pollIntervalMinutes,
                      protocol::ScheduleState scheduleState,
                      uint32_t scheduledTargetUnix);
  bool releaseActiveFlags(bool timerFlag, bool alarmFlag,
                          bool disableAlarmInterrupt);
  const char* lastError() const { return lastError_; }

 private:
  rtc::Pcf8563& rtc_;
  const char* lastError_ = "not attempted";
};

}  // namespace gathra
