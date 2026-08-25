#include "node_app.hpp"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board_pins.hpp"
#include "build_config.hpp"
#include "esp32-hal.h"
#include "firmware_version.hpp"
#include "logger.hpp"

namespace gathra {
namespace {

int16_t toCentiTemperature(float value) {
  if (!isfinite(value) || value < -327.67F || value > 327.67F)
    return kTemperatureUnavailable;
  return static_cast<int16_t>(lroundf(value * 100.0F));
}

uint16_t toCentiHumidity(float value) {
  if (!isfinite(value) || value < 0.0F || value > 100.0F)
    return kHumidityUnavailable;
  return static_cast<uint16_t>(lroundf(value * 100.0F));
}

const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN: return "UNKNOWN";
    case ESP_RST_POWERON: return "POWER_ON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "LEGACY_DEEP_SLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
  }
  return "UNKNOWN";
}

uint8_t statusFlags(const rtc::Status& status) {
  return static_cast<uint8_t>((status.timerFlag ? rtc::kControl2Tf : 0U) |
                              (status.alarmFlag ? rtc::kControl2Af : 0U));
}

}  // namespace

NodeApp::NodeApp()
    : rtc_(rtcIo_), power_(rtc_), filter_(persistent_.state().filter),
      commands_(*this) {}

void NodeApp::beginWatchdog() { enableLoopWDT(); }
void NodeApp::feedWatchdog() { feedLoopWDT(); }

void NodeApp::buildIdentity() {
  uint8_t mac[6]{};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    (void)esp_efuse_mac_get_default(mac);
  }
  snprintf(macAddress_, sizeof(macAddress_), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(defaultNodeId_, sizeof(defaultNodeId_), "GTH-%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void NodeApp::begin() {
  // GPIO2 is an active-HIGH buzzer in v2. Drive it LOW before any lengthy
  // startup operation.
  buzzer_.begin();
  Serial.begin(build::kSerialBaud);
  delay(250);
  Logger::instance().begin();
  beginWatchdog();
  buildIdentity();
  const esp_reset_reason_t resetReason = esp_reset_reason();
  GTH_LOGI("APP", "GATHRA Node firmware=%s protocol=%u build=%s MAC=%s reset=%s",
           GATHRA_FIRMWARE_VERSION, GATHRA_PROTOCOL_VERSION,
           GATHRA_BUILD_ID, macAddress_, resetReasonName(resetReason));

  // The external pull-ups intentionally do not exist. Configure the ESP32-C3
  // internal pull-ups before Wire takes ownership of GPIO8/GPIO9.
  const bool wireReady = rtcIo_.begin(board::kRtcSda, board::kRtcScl, 100000U, 50U);
  if (wireReady) (void)refreshRtc();
  else {
    pcfStatus_ = rtc::Status{};
    rtcTimeState_ = rtc::TimeState::kI2cError;
    rtcUnixTime_ = 0U;
  }
  GTH_LOGI("RTC", "PCF8563 comm=%s C2=0x%02X TF=%s AF=%s mode=%s time=%s unix=%lu",
           pcfStatus_.communicationOkay ? "yes" : "no", pcfStatus_.control2,
           pcfStatus_.timerFlag ? "1" : "0", pcfStatus_.alarmFlag ? "1" : "0",
           pcfStatus_.levelMode ? "level" : "pulse", rtc::timeStateName(rtcTimeState_),
           static_cast<unsigned long>(rtcUnixTime_));

  bool storageFormatted = false;
  const bool storagePartitionReady = initializeV2Storage(storageFormatted);
  GTH_LOGI("NVS", "partition=%s ready=%s firstFormat=%s",
           build::kStoragePartition,
           storagePartitionReady ? "yes" : "no",
           storageFormatted ? "yes" : "no");

  bool stateFresh = false;
  uint32_t generatedSession = esp_random();
  if (generatedSession == 0U) generatedSession = 1U;
  persistenceReady_ = storagePartitionReady &&
                      persistent_.begin(stateBackend_, generatedSession, stateFresh);
  GTH_LOGI("NVS", "%s session=%08lX nextSequence=%lu",
           persistent_.lastError(),
           static_cast<unsigned long>(persistent_.state().persistentSessionId),
           static_cast<unsigned long>(persistent_.state().nextSequence));
  if (persistenceReady_) {
    bool reconciled = false;
    if (!persistent_.reconcileLastFlagClearAfterColdBoot(
            resetReason == ESP_RST_POWERON, reconciled)) {
      GTH_LOGE("NVS", "%s", persistent_.lastError());
    } else if (reconciled) {
      GTH_LOGI("POWER", "%s", persistent_.lastError());
    }
  }

  const ExpectedRebootMode expected = persistenceReady_
                                          ? persistent_.state().expectedReboot
                                          : ExpectedRebootMode::kNone;
  boot_ = PowerManager::classify(
      pcfStatus_, expected, rtcTimeState_, rtcUnixTime_,
      persistent_.state().scheduleState,
      persistent_.state().scheduledMaintenanceUnix);
  // Keep the electrical boot observation separate from the logical recovery
  // classification below.  With no RTC flag asserted, a battery-powered boot
  // is being held only by the physical button and must establish the PCF timer
  // latch even when persisted maintenance state makes the logical reason a
  // maintenance recovery.
  const bool needsManualLatch =
      boot_.reason == protocol::BootReason::kManualButton;
  if (persistenceReady_ && persistent_.state().maintenanceActive &&
      expected == ExpectedRebootMode::kNone &&
      boot_.reason != protocol::BootReason::kRtcScheduledMaintenance) {
    // Recover an unexpected watchdog/panic reset inside maintenance. A TF
    // latch must not turn that reset into a fresh polling lifecycle.
    boot_.reason = protocol::BootReason::kMaintenanceReboot;
    boot_.enterMaintenance = true;
    GTH_LOGW("POWER", "recovering persisted maintenance state without an expected-reboot marker");
  }
  GTH_LOGI("POWER", "boot reason=%s expected=%s bootFlags=0x%02X",
           protocol::bootReasonName(boot_.reason), expectedRebootName(expected),
           statusFlags(pcfStatus_));

  bool manualLatchOkay = true;
  if (needsManualLatch) {
    manualLatchOkay = power_.establishManualLatch(build::kManualLatchTimeoutMs);
    if (manualLatchOkay) {
      (void)rtc_.readStatus(pcfStatus_);
      buzzer_.playBlocking(2U, 100U, 100U);
      GTH_LOGI("POWER", "%s; operator may release the physical button",
               power_.lastError());
    } else {
      GTH_LOGE("POWER", "%s; power is NOT confirmed latched",
               power_.lastError());
      buzzer_.playBlocking(3U, 100U, 100U);
    }
  }

  const bool configReady = configStore_.load(config_, defaultNodeId_);
  GTH_LOGI("CONFIG", "%s poll=%u minute(s)", configStore_.lastError(),
           config_.pollIntervalMinutes);
  historyReady_ = history_.begin(historyBackend_);
  GTH_LOGI("NVS", "%s count=%u/%u entryBytes=%u",
           history_.lastError(), history_.count(), history_.capacity(),
           static_cast<unsigned>(sizeof(HistoryRecord)));

  ota_.begin();
  const bool sonarReady = sensors_.begin();
  const bool radioReady = radio_.begin(config_);
  lastTx_.radioReady = radioReady;
  const bool internalStateSane = configReady && persistenceReady_ && historyReady_ &&
                                 pcfStatus_.communicationOkay && manualLatchOkay &&
                                 static_cast<bool>(validateConfig(config_));
  if (!ota_.completeBootValidation(internalStateSane)) {
    GTH_LOGE("OTA", "boot validation did not complete cleanly");
  }
  GTH_LOGI("APP", "peripherals RTC=%s sonar=%s radio=%s persistence=%s history=%s",
           pcfStatus_.communicationOkay ? "ready" : "error",
           sonarReady ? "ready" : "degraded", radioReady ? "ready" : "degraded",
           persistenceReady_ ? "ready" : "error", historyReady_ ? "ready" : "error");

  if (persistenceReady_) {
    PowerEvent event{};
    event.bootReason = boot_.reason;
    event.pcfStatusAtBoot = statusFlags(pcfStatus_);
    event.rtcUnixAtBoot = rtcTimeState_ == rtc::TimeState::kValid ? rtcUnixTime_ : 0U;
    event.scheduledMaintenanceUnix = persistent_.state().scheduledMaintenanceUnix;
    event.commandId = persistent_.state().command.commandId;
    (void)persistent_.appendPowerEvent(event);
    if (expected != ExpectedRebootMode::kNone) {
      persistent_.state().expectedReboot = ExpectedRebootMode::kNone;
      if (!persistent_.commit()) {
        GTH_LOGE("NVS", "could not clear consumed expected-reboot marker");
      }
    }
  }

#ifdef GATHRA_FORCE_MAINTENANCE
  boot_.enterMaintenance = true;
  maintenanceSource_ = "HIL_FORCED";
  GTH_LOGW("APP", "HIL profile forces maintenance without changing boot classification");
#endif

  nextPollMinutes_ = config_.pollIntervalMinutes;
  if (!manualLatchOkay || !pcfStatus_.communicationOkay) {
    enterFault(!manualLatchOkay ? "MANUAL_LATCH_FAILED" : "PCF8563_UNAVAILABLE");
    return;
  }
  if (boot_.alarmFlagAtBoot &&
      boot_.reason != protocol::BootReason::kRtcScheduledMaintenance &&
      persistent_.state().scheduleState == protocol::ScheduleState::kPending) {
    GTH_LOGE("POWER", "AF does not match the authoritative pending UTC target; schedule marked failed");
    persistent_.state().scheduleState = protocol::ScheduleState::kFailed;
    (void)persistent_.commit();
  }

  if (boot_.enterMaintenance) {
    if (maintenanceSource_ == nullptr || strcmp(maintenanceSource_, "NONE") == 0) {
      maintenanceSource_ = protocol::bootReasonName(boot_.reason);
    }
    enterMaintenance(maintenanceSource_);
  } else {
    state_ = AppState::kAcquire;
    buzzer_.playBlocking(1U, 100U);
  }
}

void NodeApp::run() {
  feedWatchdog();
  switch (state_) {
    case AppState::kBoot:
      state_ = AppState::kFault;
      break;
    case AppState::kAcquire:
      acquireStage();
      state_ = AppState::kFilter;
      break;
    case AppState::kFilter:
      filterStage();
      state_ = filterResult_.disposition == FilterDisposition::kNeedsVerification
                   ? AppState::kVerify
                   : AppState::kTransmit;
      break;
    case AppState::kVerify:
      verifyStage();
      state_ = AppState::kTransmit;
      break;
    case AppState::kTransmit:
      transmitStage();
      break;
    case AppState::kMaintenance:
      maintenanceLoop();
      break;
    case AppState::kPowerOff:
      // USB can keep the ESP32-C3 alive after the battery rail is genuinely
      // released. Remain inert; this state is not proof of a physical cut.
      buzzer_.stop();
      delay(20);
      break;
    case AppState::kFault:
      faultLoop();
      break;
  }
}

bool NodeApp::refreshRtc() {
  rtc::Status status{};
  rtc::DateTime dateTime{};
  uint32_t unixTime = 0U;
  if (!rtc_.readStatus(status)) {
    pcfStatus_ = rtc::Status{};
    rtcTimeState_ = rtc::TimeState::kI2cError;
    rtcUnixTime_ = 0U;
    return false;
  }
  pcfStatus_ = status;
  rtcTimeState_ = rtc_.readDateTime(dateTime, &unixTime);
  rtcUnixTime_ = rtcTimeState_ == rtc::TimeState::kValid ? unixTime : 0U;
  lastRtcRefreshMs_ = millis();
  return rtcTimeState_ != rtc::TimeState::kI2cError;
}

protocol::RtcState NodeApp::protocolRtcState() const {
  switch (rtcTimeState_) {
    case rtc::TimeState::kValid: return protocol::RtcState::kValid;
    case rtc::TimeState::kInvalidVl: return protocol::RtcState::kInvalidVl;
    case rtc::TimeState::kUninitialized: return protocol::RtcState::kUninitialized;
    case rtc::TimeState::kI2cError: return protocol::RtcState::kI2cError;
  }
  return protocol::RtcState::kI2cError;
}

void NodeApp::acquireStage() {
  measurement_ = Measurement{};
  measurement_.persistentSessionId = persistent_.state().persistentSessionId;
  if (!persistenceReady_ || !persistent_.allocateSequence(measurement_.sequence)) {
    GTH_LOGE("NVS", "measurement sequence allocation failed; this boot will not transmit");
    measurement_.healthFlags |= kRadioError;
  }
  GTH_LOGI("APP", "ACQUIRE session=%08lX sequence=%lu",
           static_cast<unsigned long>(measurement_.persistentSessionId),
           static_cast<unsigned long>(measurement_.sequence));
  measurement_.battery = sensors_.readBattery(config_);
  measurement_.environment = sensors_.readEnvironment();
  measurement_.sonar = sensors_.readSonarBurst(config_, measurement_.environment);
  historyPending_ = measurement_.sequence != 0U;
  prepareHealthAfterAcquisition();
}

void NodeApp::prepareHealthAfterAcquisition() {
  const uint16_t preserved = measurement_.healthFlags;
  measurement_.healthFlags = preserved;
  measurement_.qualityFlags = kQualityNone;
  if (!measurement_.environment.sensorReadOk) measurement_.healthFlags |= kDhtInvalid;
  if (!measurement_.environment.valid) measurement_.healthFlags |= kEnvironmentStale;
  if (!measurement_.environment.sensorReadOk || !measurement_.environment.valid)
    measurement_.healthFlags |= kSensorDegraded;
  if (measurement_.environment.valid) measurement_.qualityFlags |= kEnvironmentCompensated;
  if (!measurement_.sonar.valid)
    measurement_.healthFlags |= kSonarInvalid | kSensorDegraded;
  else
    measurement_.qualityFlags |= kRawDistanceValid;
  if (config_.installationMinimumDistanceMm != 0U)
    measurement_.qualityFlags |= kInstallationLimitsApplied;
  if (!measurement_.battery.adcValid) measurement_.healthFlags |= kBatteryAdcInvalid;
  if (measurement_.battery.low) measurement_.healthFlags |= kBatteryLow;
  if (measurement_.battery.critical) measurement_.healthFlags |= kBatteryCritical;
  if (config_.referenceDistanceMm == 0U) measurement_.healthFlags |= kCalibrationMissing;
}

void NodeApp::filterStage() {
  filterResult_ = filter_.evaluate(measurement_.sonar.medianDistanceMm,
                                   measurement_.sonar.valid, config_);
  applyFilterResult(filterResult_);
  GTH_LOGI("FILTER", "state=%s raw=%lu accepted=%lu verify=%s",
           filterStateName(filterResult_.state),
           static_cast<unsigned long>(measurement_.sonar.medianDistanceMm),
           static_cast<unsigned long>(measurement_.acceptedDistanceMm),
           filterResult_.disposition == FilterDisposition::kNeedsVerification ? "yes" : "no");
}

bool NodeApp::waitVerificationInterval(uint16_t intervalMs) {
  const uint32_t started = millis();
  while (millis() - started < intervalMs) {
    feedWatchdog();
    delay(20);
  }
  return true;
}

void NodeApp::verifyStage() {
  measurement_.qualityFlags |= kVerificationPerformed;
  const uint8_t target = filter_.verificationTarget(config_);
  const uint16_t interval = filter_.verificationIntervalMs(config_);
  while (filter_.candidateActive() &&
         persistent_.state().filter.candidateObservations < target) {
    (void)waitVerificationInterval(interval);
    const SonarBurst verification =
        sensors_.readSonarBurst(config_, measurement_.environment);
    filterResult_ = filter_.observeVerification(verification.medianDistanceMm,
                                                verification.valid, config_);
  }
  if (filter_.candidateActive()) filterResult_ = filter_.finishVerification(config_);
  applyFilterResult(filterResult_);
  GTH_LOGI("FILTER", "verification result=%s votes=%u/%u accepted=%lu",
           filterStateName(filterResult_.state),
           persistent_.state().filter.lastCandidateVotes,
           persistent_.state().filter.lastCandidateObservations,
           static_cast<unsigned long>(measurement_.acceptedDistanceMm));
}

void NodeApp::applyFilterResult(const FilterResult& result) {
  measurement_.filterState = result.state;
  measurement_.acceptedDistanceMm = result.acceptedDistanceMm;
  measurement_.candidateDistanceMm = result.candidateDistanceMm;
  measurement_.healthFlags &= ~(kFilterTransient | kFilterUncertain);
  if (result.state == FilterState::kTransientRejected)
    measurement_.healthFlags |= kFilterTransient;
  if (result.state == FilterState::kUncertain || result.state == FilterState::kInvalid)
    measurement_.healthFlags |= kFilterUncertain;
  if (measurement_.acceptedDistanceMm != kDistanceUnavailable)
    measurement_.qualityFlags |= kAcceptedDistanceValid;
  updateDerivedHeight();
  // PCF8563 normal hard-power wake granularity is one minute. A suspicious
  // result requests the minimum honest one-minute off interval; fast rise/fall
  // verification already happened within this boot.
  nextPollMinutes_ = result.scheduleSoon ? 1U : config_.pollIntervalMinutes;
}

void NodeApp::updateDerivedHeight() {
  if (config_.referenceDistanceMm == 0U ||
      measurement_.acceptedDistanceMm == kDistanceUnavailable) {
    measurement_.derivedWaterHeightMm = kHeightUnavailable;
    return;
  }
  measurement_.derivedWaterHeightMm =
      static_cast<int32_t>(config_.referenceDistanceMm) -
      static_cast<int32_t>(measurement_.acceptedDistanceMm);
}

protocol::TelemetryPacket NodeApp::makeTelemetry() {
  (void)refreshRtc();
  protocol::TelemetryPacket packet{};
  strncpy(packet.nodeId, config_.nodeId, sizeof(packet.nodeId) - 1U);
  packet.persistentSessionId = measurement_.persistentSessionId;
  packet.sequence = measurement_.sequence;
  packet.medianEchoUs = measurement_.sonar.medianEchoUs;
  packet.rawDistanceMm = measurement_.sonar.medianDistanceMm;
  packet.acceptedDistanceMm = measurement_.acceptedDistanceMm;
  packet.madMm = measurement_.sonar.madMm;
  packet.temperatureCentiC = toCentiTemperature(measurement_.environment.temperatureC);
  packet.humidityCentiPercent = toCentiHumidity(measurement_.environment.humidityPercent);
  packet.batteryMv = measurement_.battery.batteryMillivolts;
  packet.validSamples = measurement_.sonar.validSamples;
  packet.totalSamples = measurement_.sonar.totalSamples;
  packet.filterState = measurement_.filterState;
  packet.qualityFlags = measurement_.qualityFlags;
  packet.healthFlags = measurement_.healthFlags;
  packet.bootReason = boot_.reason;
  packet.rtcState = protocolRtcState();
  packet.rtcUnixTime = rtcTimeState_ == rtc::TimeState::kValid ? rtcUnixTime_ : 0U;
  packet.pollIntervalMinutes = config_.pollIntervalMinutes;
  packet.scheduleState = persistent_.state().scheduleState;
  packet.scheduledMaintenanceUnix = persistent_.state().scheduledMaintenanceUnix;
  packet.lastCommandId = persistent_.state().command.commandId;
  packet.lastCommandType = persistent_.state().command.commandType;
  packet.lastCommandResult = persistent_.state().command.result;
  return packet;
}

void NodeApp::synchronizeRtc(const protocol::AckCommandPacket& ack) {
  if (!ack.timeValid || ack.gatewayUnixTime == 0U) return;
  rtc::DateTime gatewayDate{};
  if (!rtc::Pcf8563::unixToDateTime(ack.gatewayUnixTime, gatewayDate)) {
    GTH_LOGW("RTC", "trusted Gateway UTC is outside PCF8563 supported 2000-2099 range");
    return;
  }
  (void)refreshRtc();
  bool writeRequired = rtcTimeState_ != rtc::TimeState::kValid;
  if (!writeRequired) {
    const uint32_t drift = rtcUnixTime_ > ack.gatewayUnixTime
                               ? rtcUnixTime_ - ack.gatewayUnixTime
                               : ack.gatewayUnixTime - rtcUnixTime_;
    writeRequired = drift > build::kRtcSyncDriftThresholdSec;
    GTH_LOGI("RTC", "Gateway UTC trusted drift=%lu s write=%s",
             static_cast<unsigned long>(drift), writeRequired ? "yes" : "no");
  }
  if (writeRequired && !rtc_.writeDateTime(gatewayDate, true)) {
    GTH_LOGE("RTC", "trusted UTC write/readback failed");
    (void)refreshRtc();
    return;
  }
  rtcUnixTime_ = ack.gatewayUnixTime;
  rtcTimeState_ = rtc::TimeState::kValid;
  persistent_.state().lastGatewayUtc = ack.gatewayUnixTime;
  persistent_.state().lastRtcSyncUnix = ack.gatewayUnixTime;
  if (state_ == AppState::kMaintenance &&
      persistent_.state().maintenanceActive &&
      persistent_.state().maintenanceDeadlineUnix == 0U) {
    // A manual boot may begin while VL is set. Once a trusted ACK repairs UTC,
    // anchor the remaining fixed lifetime so later OTA/watchdog reboots cannot
    // restart the energy budget.
    persistent_.state().maintenanceDeadlineUnix =
        ack.gatewayUnixTime + maintenanceRemainingSec();
  }
  GTH_LOGI("RTC", "UTC synchronized from Gateway unix=%lu",
           static_cast<unsigned long>(ack.gatewayUnixTime));
}

void NodeApp::transmitStage() {
  protocol::AckCommandPacket ack{};
  if (measurement_.sequence != 0U) {
    GTH_LOGI("APP", "TRANSMIT sequence=%lu",
             static_cast<unsigned long>(measurement_.sequence));
    lastTx_ = radio_.sendTelemetry(makeTelemetry(), config_, &ack);
  } else {
    lastTx_ = TxReport{};
    lastTx_.radioReady = radio_.ready();
  }
  if (!lastTx_.radioReady || !lastTx_.transmitted)
    measurement_.healthFlags |= kRadioError;
  if (!lastTx_.acknowledged) measurement_.healthFlags |= kTxUnacked;

  if (lastTx_.acknowledged) {
    synchronizeRtc(ack);
    const CommandApplyResult command = commands_.handle(
        ack, config_.nodeId, persistent_.state().persistentSessionId);
    if (command.sendResult) {
      lastTx_.commandResultTransmitted = radio_.sendCommandResult(command.packet);
      if (!lastTx_.commandResultTransmitted) measurement_.healthFlags |= kRadioError;
    }
    commandEnterMaintenance_ = command.enterMaintenance;
  }

  const bool historyOkay = appendCurrentHistory();
  const bool stateOkay = persistenceReady_ && persistent_.commit();
  if (!historyOkay || !stateOkay) measurement_.healthFlags |= kSensorDegraded;

  if (commandEnterMaintenance_) {
    enterMaintenance("ACK_COMMAND");
    return;
  }
  const bool success = lastTx_.acknowledged && historyOkay && stateOkay;
  buzzer_.playBlocking(success ? 2U : 3U, 100U, 100U);
  (void)finalPowerOff(success ? ShutdownReason::kPollSuccess
                              : ShutdownReason::kPollError,
                      false);
}

bool NodeApp::appendCurrentHistory() {
  if (!historyPending_) return true;
  if (!historyReady_) return false;
  HistoryEntry entry{};
  entry.sequence = measurement_.sequence;
  entry.rtcUnixTime = rtcTimeState_ == rtc::TimeState::kValid ? rtcUnixTime_ : 0U;
  entry.rawDistanceMm = measurement_.sonar.medianDistanceMm;
  entry.acceptedDistanceMm = measurement_.acceptedDistanceMm;
  entry.madMm = measurement_.sonar.madMm;
  entry.batteryMv = measurement_.battery.batteryMillivolts;
  entry.temperatureCentiC = toCentiTemperature(measurement_.environment.temperatureC);
  entry.humidityCentiPercent = toCentiHumidity(measurement_.environment.humidityPercent);
  entry.qualityFlags = measurement_.qualityFlags;
  entry.healthFlags = measurement_.healthFlags;
  entry.filterState = static_cast<uint8_t>(measurement_.filterState);
  entry.validSamples = measurement_.sonar.validSamples;
  entry.totalSamples = measurement_.sonar.totalSamples;
  const bool saved = history_.append(entry);
  if (saved) historyPending_ = false;
  else GTH_LOGE("NVS", "%s", history_.lastError());
  return saved;
}

void NodeApp::enterMaintenance(const char* source) {
  maintenanceSource_ = source == nullptr ? "UNKNOWN" : source;
  const uint32_t configuredMs =
      static_cast<uint32_t>(config_.maintenanceTimeoutSec) * 1000U;
  maintenanceLifetimeMs_ = configuredMs > build::kMaintenanceMaximumSec * 1000U
                               ? build::kMaintenanceMaximumSec * 1000U
                               : configuredMs;
  maintenanceStartedMs_ = millis();
  if (persistenceReady_) {
    if (persistent_.state().maintenanceActive) {
      if (persistent_.state().maintenanceDeadlineUnix == 0U ||
          rtcTimeState_ != rtc::TimeState::kValid ||
          rtcUnixTime_ >= persistent_.state().maintenanceDeadlineUnix) {
        // A rebooted maintenance lifetime without a trustworthy remaining
        // deadline cannot safely be restarted. Program/release immediately.
        maintenanceLifetimeMs_ = 0U;
      } else {
        const uint32_t remaining =
            (persistent_.state().maintenanceDeadlineUnix - rtcUnixTime_) * 1000U;
        if (remaining < maintenanceLifetimeMs_) maintenanceLifetimeMs_ = remaining;
      }
    } else {
      persistent_.state().maintenanceActive = true;
      persistent_.state().maintenanceDeadlineUnix =
          rtcTimeState_ == rtc::TimeState::kValid
              ? rtcUnixTime_ + maintenanceLifetimeMs_ / 1000U
              : 0U;
      (void)persistent_.commit();
    }
  }
  if (!portal_.running() && !portal_.begin(*this)) {
    GTH_LOGE("WEB", "maintenance portal unavailable");
    (void)finishMaintenance(ShutdownReason::kRecovery);
    return;
  }
  state_ = AppState::kMaintenance;
  GTH_LOGI("POWER", "maintenance source=%s fixedLifetimeMs=%lu deadlineUtc=%lu",
           maintenanceSource_, static_cast<unsigned long>(maintenanceLifetimeMs_),
           static_cast<unsigned long>(persistent_.state().maintenanceDeadlineUnix));
}

uint32_t NodeApp::maintenanceRemainingSec() const {
  if (state_ != AppState::kMaintenance) return 0U;
  const uint32_t elapsed = millis() - maintenanceStartedMs_;
  if (elapsed >= maintenanceLifetimeMs_) return 0U;
  return (maintenanceLifetimeMs_ - elapsed + 999U) / 1000U;
}

void NodeApp::maintenanceLoop() {
  portal_.loop();
  buzzer_.maintenanceTick(millis());
  if (millis() - lastRtcRefreshMs_ >= 1000U) (void)refreshRtc();
  if (rebootRequested_) {
    portal_.stop();
    radio_.sleep();
    buzzer_.stop();
    GTH_LOGI("APP", "software reboot with RTC latch intentionally retained");
    Serial.flush();
    delay(100);
    ESP.restart();
  }
  if (exitMaintenanceRequested_) {
    exitMaintenanceRequested_ = false;
    (void)finishMaintenance(ShutdownReason::kMaintenanceExit);
    return;
  }
  if (millis() - maintenanceStartedMs_ >= maintenanceLifetimeMs_) {
    GTH_LOGI("POWER", "fixed maintenance energy deadline reached");
    (void)finishMaintenance(ShutdownReason::kMaintenanceExpired);
    return;
  }
  delay(2);
}

bool NodeApp::finishMaintenance(ShutdownReason reason) {
  if (persistenceReady_) {
    persistent_.state().maintenanceActive = false;
    persistent_.state().maintenanceDeadlineUnix = 0U;
    // OTA/maintenance reboot markers intentionally take boot-reason
    // precedence, but they must not erase the origin of an AF-held one-shot
    // maintenance window. Re-evaluate the retained alarm without the reboot
    // override before completing the durable schedule.
    (void)refreshRtc();
    const BootClassification rtcEvidence = PowerManager::classify(
        pcfStatus_, ExpectedRebootMode::kNone, rtcTimeState_, rtcUnixTime_,
        persistent_.state().scheduleState,
        persistent_.state().scheduledMaintenanceUnix);
    if (boot_.reason == protocol::BootReason::kRtcScheduledMaintenance ||
        rtcEvidence.reason == protocol::BootReason::kRtcScheduledMaintenance) {
      persistent_.state().scheduleState = protocol::ScheduleState::kCompleted;
    }
    if (!persistent_.commit()) {
      enterFault("MAINTENANCE_STATE_PERSIST_FAILED");
      return false;
    }
  }
  return finalPowerOff(reason, true);
}

PowerEvent* NodeApp::currentPowerEvent() {
  PersistentState& state = persistent_.state();
  if (state.powerEventCount == 0U) return nullptr;
  const uint8_t index = state.powerEventHead == 0U ? 7U : state.powerEventHead - 1U;
  return &state.powerEvents[index];
}

bool NodeApp::finalPowerOff(ShutdownReason reason, bool maintenanceFinishBeep) {
  powerOffReadiness_ = "PERSISTING";
  portal_.stop();
  if (!persistenceReady_ || !persistent_.commit()) {
    enterFault("CRITICAL_STATE_PERSIST_FAILED");
    return false;
  }
  if (historyPending_ && !appendCurrentHistory()) {
    enterFault("HISTORY_PERSIST_FAILED");
    return false;
  }

  powerOffReadiness_ = "PROGRAMMING_NEXT_WAKE";
  bool programmed = false;
  for (uint8_t attempt = 1U; attempt <= 3U && !programmed; ++attempt) {
    programmed = power_.programNextWake(
        nextPollMinutes_, persistent_.state().scheduleState,
        persistent_.state().scheduledMaintenanceUnix);
    if (!programmed) {
      GTH_LOGE("POWER", "next wake attempt=%u failed: %s", attempt,
               power_.lastError());
      feedWatchdog();
      delay(50);
    }
  }
  if (!programmed) {
    if (PowerEvent* event = currentPowerEvent()) {
      event->shutdownReason = ShutdownReason::kPowerProgrammingFault;
      event->timerMinutes = nextPollMinutes_;
      (void)persistent_.commit();
    }
    enterFault("NEXT_WAKE_VERIFY_FAILED_DO_NOT_POWER_OFF");
    return false;
  }

  (void)refreshRtc();
  nextExpectedPollUnix_ = rtcTimeState_ == rtc::TimeState::kValid
                              ? rtcUnixTime_ +
                                    static_cast<uint32_t>(nextPollMinutes_) * 60U
                              : 0U;
  if (pcfStatus_.alarmFlag &&
      persistent_.state().scheduleState == protocol::ScheduleState::kPending) {
    const uint32_t target = persistent_.state().scheduledMaintenanceUnix;
    if (rtcTimeState_ == rtc::TimeState::kValid && rtcUnixTime_ >= target &&
        rtcUnixTime_ - target <= 300U) {
      GTH_LOGW("POWER", "maintenance alarm became due during operation; preserving AF and entering maintenance");
      // Treat the new maintenance lifetime as the scheduled one-shot so its
      // eventual completion transitions the persistent schedule to COMPLETED.
      boot_.reason = protocol::BootReason::kRtcScheduledMaintenance;
      boot_.alarmFlagAtBoot = true;
      enterMaintenance("RTC_ALARM_DURING_OPERATION");
      return false;
    }
    GTH_LOGE("POWER", "unexpected AF cannot safely represent pending target; schedule marked failed");
    persistent_.state().scheduleState = protocol::ScheduleState::kFailed;
    if (!persistent_.commit() ||
        !power_.programNextWake(nextPollMinutes_, protocol::ScheduleState::kFailed,
                                persistent_.state().scheduledMaintenanceUnix) ||
        !refreshRtc()) {
      enterFault("UNEXPECTED_AF_RECOVERY_FAILED");
      return false;
    }
  }

  if (PowerEvent* event = currentPowerEvent()) {
    event->shutdownReason = reason;
    event->timerMinutes = nextPollMinutes_;
    event->scheduledMaintenanceUnix = persistent_.state().scheduledMaintenanceUnix;
    event->flagClearAttempted = true;
    event->flagClearSucceeded = false;
    if (!persistent_.commit()) {
      enterFault("POWER_DIAGNOSTIC_PERSIST_FAILED");
      return false;
    }
  }

  radio_.sleep();
  WiFi.mode(WIFI_OFF);
  powerOffReadiness_ = "WAKE_VERIFIED_FLAG_CLEAR_PENDING";
  GTH_LOGI("POWER", "next timer=%u min alarm=%s target=%lu verified; final flag clear is point-of-no-return",
           nextPollMinutes_,
           persistent_.state().scheduleState == protocol::ScheduleState::kPending
               ? "armed"
               : "disabled",
           static_cast<unsigned long>(persistent_.state().scheduledMaintenanceUnix));
  if (maintenanceFinishBeep) buzzer_.playBlocking(1U, 300U);
  buzzer_.stop();
  Serial.flush();

  const bool timerFlag = pcfStatus_.timerFlag;
  const bool alarmFlag = pcfStatus_.alarmFlag;
  const bool completedAlarm =
      persistent_.state().scheduleState != protocol::ScheduleState::kPending;
  if (!power_.releaseActiveFlags(timerFlag, alarmFlag, completedAlarm)) {
    GTH_LOGE("POWER", "final release aborted/failed: %s", power_.lastError());
    if (PowerEvent* event = currentPowerEvent()) {
      event->flagClearSucceeded = false;
      (void)persistent_.commit();
    }
    enterFault("FINAL_FLAG_CLEAR_FAILED");
    return false;
  }

  powerOffReadiness_ = "FLAG_RELEASED_EXPECT_POWER_LOSS";
  // This write normally cannot complete on battery because AO3401A removes
  // power immediately. It is useful under USB HIL only and is not treated as
  // proof of real hard power-off.
  if (PowerEvent* event = currentPowerEvent()) {
    event->flagClearSucceeded = true;
    (void)persistent_.commit();
  }
  GTH_LOGI("POWER", "RTC latch flag released; USB may keep ESP alive (hard power cut requires no-USB HIL)");
  Serial.flush();
  state_ = AppState::kPowerOff;
  return true;
}

void NodeApp::enterFault(const char* reason) {
  powerOffReadiness_ = reason == nullptr ? "FAULT" : reason;
  state_ = AppState::kFault;
  faultRetryAtMs_ = millis() + 30000U;
  buzzer_.playBlocking(3U, 100U, 100U);
  GTH_LOGE("POWER", "bounded recovery fault=%s; active RTC flag will NOT be cleared",
           powerOffReadiness_);
  if (!portal_.running() && config_.nodeId[0] != '\0') (void)portal_.begin(*this);
}

void NodeApp::faultLoop() {
  portal_.loop();
  buzzer_.maintenanceTick(millis());
  if (millis() - lastRtcRefreshMs_ >= 1000U) (void)refreshRtc();
  if (rebootRequested_) {
    portal_.stop();
    buzzer_.stop();
    Serial.flush();
    ESP.restart();
  }
  if (static_cast<int32_t>(millis() - faultRetryAtMs_) >= 0) {
    faultRetryAtMs_ = millis() + 30000U;
    GTH_LOGW("POWER", "fault recovery retry: program and verify next normal wake");
    if (pcfStatus_.communicationOkay && persistenceReady_ &&
        power_.programNextWake(config_.pollIntervalMinutes,
                               persistent_.state().scheduleState,
                               persistent_.state().scheduledMaintenanceUnix)) {
      nextPollMinutes_ = config_.pollIntervalMinutes;
      (void)finalPowerOff(ShutdownReason::kRecovery, true);
    }
  }
  delay(5);
}

bool NodeApp::measureNow(String& error) {
  const AppState prior = state_;
  acquireStage();
  filterStage();
  if (filterResult_.disposition == FilterDisposition::kNeedsVerification) verifyStage();
  const bool okay = appendCurrentHistory() && persistent_.commit();
  state_ = prior;
  error = okay ? "" : "measurement/filter/history NVS persistence failed";
  return okay;
}

bool NodeApp::applyConfiguration(const NodeConfig& candidate, String& error) {
  const ConfigValidationResult validation = validateConfig(candidate);
  if (!validation) {
    error = validation.message;
    return false;
  }
  const NodeConfig previous = config_;
  const bool radioChanged = !radioConfigEqual(previous, candidate);
  if (radioChanged && !radio_.applyConfig(candidate)) {
    (void)radio_.applyConfig(previous);
    error = "radio rejected candidate settings; previous configuration restored";
    return false;
  }
  if (!configStore_.save(candidate)) {
    if (radioChanged) (void)radio_.applyConfig(previous);
    error = configStore_.lastError();
    return false;
  }
  config_ = candidate;
  nextPollMinutes_ = config_.pollIntervalMinutes;
  updateDerivedHeight();
  error = "";
  return true;
}

bool NodeApp::captureCalibration(String& error) {
  const bool stable = measurement_.acceptedDistanceMm != kDistanceUnavailable &&
      (measurement_.filterState == FilterState::kStable ||
       measurement_.filterState == FilterState::kAccepted ||
       measurement_.filterState == FilterState::kChangeConfirmed);
  if (!stable) {
    error = "capture requires a current stable/accepted measurement";
    return false;
  }
  return setCalibration(measurement_.acceptedDistanceMm, error);
}

bool NodeApp::setCalibration(uint32_t referenceDistanceMm, String& error) {
  NodeConfig candidate = config_;
  candidate.referenceDistanceMm = referenceDistanceMm;
  return applyConfiguration(candidate, error);
}

TxReport NodeApp::sendRadioTest() {
  uint32_t sequence = 0U;
  if (!persistent_.allocateSequence(sequence)) {
    lastTx_ = TxReport{};
    return lastTx_;
  }
  measurement_.persistentSessionId = persistent_.state().persistentSessionId;
  measurement_.sequence = sequence;
  protocol::AckCommandPacket ack{};
  lastTx_ = radio_.sendTelemetry(makeTelemetry(), config_, &ack);
  if (lastTx_.acknowledged) {
    synchronizeRtc(ack);
    // A maintenance-initiated telemetry packet is still a real Protocol v2
    // exchange.  Commands in its ACK must have exactly the same durable,
    // idempotent semantics as commands received during the normal poll path.
    const CommandApplyResult command = commands_.handle(
        ack, config_.nodeId, persistent_.state().persistentSessionId);
    if (command.sendResult) {
      lastTx_.commandResultTransmitted = radio_.sendCommandResult(command.packet);
    }
    if (command.enterMaintenance && state_ != AppState::kMaintenance) {
      enterMaintenance("ACK_COMMAND");
    }
    // synchronizeRtc() updates sync metadata; keep it across hard power loss
    // even when the ACK did not contain a command.
    if (persistenceReady_ && !persistent_.commit()) {
      GTH_LOGE("NVS", "radio-test ACK metadata persistence failed");
    }
  }
  return lastTx_;
}

bool NodeApp::prepareReboot(ExpectedRebootMode mode, String& error) {
  if (mode == ExpectedRebootMode::kNone || !persistenceReady_) {
    error = "persistent reboot marker is unavailable";
    return false;
  }
  (void)refreshRtc();
  const bool latchActive = (pcfStatus_.timerFlag && pcfStatus_.timerInterruptEnabled) ||
                           (pcfStatus_.alarmFlag && pcfStatus_.alarmInterruptEnabled);
  if (!latchActive || !pcfStatus_.levelMode) {
    error = "RTC level latch is not active; reboot refused";
    return false;
  }
  const ExpectedRebootMode priorExpected = persistent_.state().expectedReboot;
  const bool priorMaintenanceActive = persistent_.state().maintenanceActive;
  const uint32_t priorDeadline = persistent_.state().maintenanceDeadlineUnix;
  persistent_.state().maintenanceActive = true;
  if (persistent_.state().maintenanceDeadlineUnix == 0U &&
      rtcTimeState_ == rtc::TimeState::kValid) {
    persistent_.state().maintenanceDeadlineUnix =
        rtcUnixTime_ + maintenanceRemainingSec();
  }
  if (persistent_.state().maintenanceDeadlineUnix == 0U ||
      rtcTimeState_ != rtc::TimeState::kValid) {
    persistent_.state().maintenanceActive = priorMaintenanceActive;
    persistent_.state().maintenanceDeadlineUnix = priorDeadline;
    error = "trusted RTC deadline unavailable; reboot refused to preserve the fixed maintenance budget";
    return false;
  }
  persistent_.state().expectedReboot = mode;
  if (!persistent_.commit()) {
    persistent_.state().expectedReboot = priorExpected;
    persistent_.state().maintenanceActive = priorMaintenanceActive;
    persistent_.state().maintenanceDeadlineUnix = priorDeadline;
    error = "expected-reboot marker NVS write failed";
    return false;
  }
  error = "";
  return true;
}

bool NodeApp::persistReceipt(const StoredCommand& command) {
  if (!persistenceReady_) return false;
  persistent_.state().command = command;
  return persistent_.commit();
}

protocol::CommandResultCode NodeApp::applyEnterMaintenance() {
  return protocol::CommandResultCode::kApplied;
}

protocol::CommandResultCode NodeApp::applyPollInterval(uint8_t minutes) {
  if (minutes == 0U) return protocol::CommandResultCode::kInvalidArgument;
  NodeConfig candidate = config_;
  candidate.pollIntervalMinutes = minutes;
  String error;
  if (!applyConfiguration(candidate, error)) {
    GTH_LOGE("COMMAND", "poll interval persistence failed: %s", error.c_str());
    return protocol::CommandResultCode::kStorageError;
  }
  return protocol::CommandResultCode::kApplied;
}

protocol::CommandResultCode NodeApp::applyMaintenanceSchedule(uint32_t targetUnix) {
  if (!refreshRtc() || !pcfStatus_.communicationOkay)
    return protocol::CommandResultCode::kRtcUnavailable;
  if (rtcTimeState_ != rtc::TimeState::kValid)
    return protocol::CommandResultCode::kRtcTimeUntrusted;
  if (!rtc::Pcf8563::scheduleRepresentable(rtcUnixTime_, targetUnix))
    return protocol::CommandResultCode::kScheduleUnrepresentable;
  persistent_.state().scheduleState = protocol::ScheduleState::kPending;
  persistent_.state().scheduledMaintenanceUnix = targetUnix;
  if (!persistent_.commit()) return protocol::CommandResultCode::kStorageError;
  rtc::DateTime target{};
  if (!rtc::Pcf8563::unixToDateTime(targetUnix, target) ||
      !rtc_.configureAlarmForUtc(target, true, true)) {
    persistent_.state().scheduleState = protocol::ScheduleState::kFailed;
    (void)persistent_.commit();
    return protocol::CommandResultCode::kInternalError;
  }
  return protocol::CommandResultCode::kApplied;
}

bool NodeApp::persistResult(const StoredCommand& command) {
  if (!persistenceReady_) return false;
  persistent_.state().command = command;
  return persistent_.commit();
}

}  // namespace gathra
