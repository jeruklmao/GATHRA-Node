#include "power_manager.hpp"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace gathra {

BootClassification PowerManager::classify(
    const rtc::Status& status, ExpectedRebootMode expectedReboot,
    rtc::TimeState timeState, uint32_t rtcUnix,
    protocol::ScheduleState scheduleState, uint32_t scheduledTargetUnix) {
  BootClassification result{};
  result.timerFlagAtBoot = status.timerFlag;
  result.alarmFlagAtBoot = status.alarmFlag;
  if (expectedReboot == ExpectedRebootMode::kOta) {
    result.reason = protocol::BootReason::kOtaReboot;
    result.enterMaintenance = true;
    return result;
  }
  if (expectedReboot == ExpectedRebootMode::kMaintenance) {
    result.reason = protocol::BootReason::kMaintenanceReboot;
    result.enterMaintenance = true;
    return result;
  }
  const bool scheduleMatches = status.alarmFlag &&
      timeState == rtc::TimeState::kValid &&
      scheduleState == protocol::ScheduleState::kPending &&
      scheduledTargetUnix != 0U && rtcUnix >= scheduledTargetUnix &&
      rtcUnix - scheduledTargetUnix <= 300U;
  if (scheduleMatches) {
    result.reason = protocol::BootReason::kRtcScheduledMaintenance;
    result.enterMaintenance = true;
    return result;
  }
  if (status.timerFlag) {
    result.reason = protocol::BootReason::kRtcTimer;
    return result;
  }
  if (status.communicationOkay && !status.timerFlag && !status.alarmFlag) {
    result.reason = protocol::BootReason::kManualButton;
    result.enterMaintenance = true;
    return result;
  }
  result.reason = protocol::BootReason::kUnknown;
  return result;
}

bool PowerManager::establishManualLatch(uint32_t timeoutMs) {
  rtc::TimerConfiguration timer{};
  timer.enabled = true;
  timer.source = rtc::TimerSource::k64Hz;
  timer.value = 8U;  // 125 ms, long enough for verified configuration readback.
  timer.interruptEnabled = true;
  timer.levelMode = true;
  if (!rtc_.configureTimer(timer, true, true)) {
    lastError_ = "64 Hz manual latch timer configuration/readback failed";
    return false;
  }
#ifdef ARDUINO
  const uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    rtc::Status status{};
    if (rtc_.readStatus(status) && status.timerFlag &&
        status.timerInterruptEnabled && status.levelMode) {
      lastError_ = "manual RTC level latch established";
      return true;
    }
    delay(5);
  }
#else
  (void)timeoutMs;
#endif
  lastError_ = "manual latch timer did not assert TF before deadline";
  return false;
}

bool PowerManager::programNextWake(uint8_t pollIntervalMinutes,
                                   protocol::ScheduleState scheduleState,
                                   uint32_t scheduledTargetUnix) {
  if (pollIntervalMinutes == 0U) {
    lastError_ = "poll interval is zero";
    return false;
  }
  // Rearm the normal wake first, always preserving TF and AF. In particular,
  // a scheduled-maintenance boot may be powered solely by AF: changing AIE or
  // clearing AF before this transaction's final write would cut power here.
  rtc::TimerConfiguration timer{};
  timer.enabled = true;
  timer.source = rtc::TimerSource::kOnePerMinute;
  timer.value = pollIntervalMinutes;
  timer.interruptEnabled = true;
  timer.levelMode = true;
  if (!rtc_.configureTimer(timer, true, true)) {
    lastError_ = "normal minute timer configuration/readback failed";
    return false;
  }

  if (scheduleState == protocol::ScheduleState::kPending) {
    rtc::DateTime target{};
    if (scheduledTargetUnix == 0U ||
        !rtc::Pcf8563::unixToDateTime(scheduledTargetUnix, target) ||
        !rtc_.configureAlarmForUtc(target, true, true)) {
      lastError_ = "pending maintenance alarm configuration/readback failed";
      return false;
    }
  } else {
    rtc::Status status{};
    if (!rtc_.readStatus(status)) {
      lastError_ = "alarm latch status read failed";
      return false;
    }
    // If AF is the current power latch, AIE must stay enabled until the final
    // Control_status_2 release write. Alarm registers are disabled safely on
    // a later TF-held boot.
    if (!status.alarmFlag && !rtc_.disableAlarm(true, true)) {
      lastError_ = "unused maintenance alarm could not be disabled";
      return false;
    }
  }
  return verifyNextWake(pollIntervalMinutes, scheduleState,
                        scheduledTargetUnix);
}

bool PowerManager::verifyNextWake(uint8_t pollIntervalMinutes,
                                  protocol::ScheduleState scheduleState,
                                  uint32_t scheduledTargetUnix) {
  rtc::TimerConfiguration timer{};
  if (!rtc_.readTimer(timer) || !timer.enabled ||
      timer.source != rtc::TimerSource::kOnePerMinute ||
      timer.value == 0U || timer.value > pollIntervalMinutes ||
      !timer.interruptEnabled || !timer.levelMode) {
    lastError_ = "normal timer verification failed";
    return false;
  }
  rtc::AlarmConfiguration alarm{};
  rtc::Status status{};
  if (!rtc_.readAlarm(alarm) || !rtc_.readStatus(status)) {
    lastError_ = "alarm verification read failed";
    return false;
  }
  if (scheduleState == protocol::ScheduleState::kPending) {
    rtc::DateTime target{};
    if (!rtc::Pcf8563::unixToDateTime(scheduledTargetUnix, target) ||
        !alarm.minuteEnabled || !alarm.hourEnabled || !alarm.dayEnabled ||
        alarm.weekdayEnabled || !alarm.interruptEnabled ||
        alarm.minute != target.minute || alarm.hour != target.hour ||
        alarm.day != target.day) {
      lastError_ = "pending alarm readback does not match target";
      return false;
    }
  } else {
    const bool alarmRegistersDisabled =
        !alarm.minuteEnabled && !alarm.hourEnabled && !alarm.dayEnabled &&
        !alarm.weekdayEnabled && !alarm.interruptEnabled;
    const bool activeAlarmDeferredToFinalRelease =
        status.alarmFlag && status.alarmInterruptEnabled;
    if (!alarmRegistersDisabled && !activeAlarmDeferredToFinalRelease) {
      lastError_ = "unused alarm is neither disabled nor a preserved active latch";
      return false;
    }
  }
  lastError_ = "next timer/alarm wake sources verified";
  return true;
}

bool PowerManager::releaseActiveFlags(bool timerFlag, bool alarmFlag,
                                      bool disableAlarmInterrupt) {
  if (!timerFlag && !alarmFlag) {
    lastError_ = "no active RTC latch flag to release";
    return false;
  }
  if (!rtc_.releaseInterruptFlags(timerFlag, alarmFlag,
                                  disableAlarmInterrupt)) {
    lastError_ = "final RTC flag clear/readback failed";
    return false;
  }
  lastError_ = "active RTC latch flag(s) released";
  return true;
}

}  // namespace gathra
