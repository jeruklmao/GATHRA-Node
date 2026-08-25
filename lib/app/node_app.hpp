#pragma once

#include <Arduino.h>

#include "buzzer.hpp"
#include "command_processor.hpp"
#include "config_store.hpp"
#include "filter.hpp"
#include "lora_radio.hpp"
#include "maintenance_portal.hpp"
#include "model.hpp"
#include "nvs_history.hpp"
#include "ota_manager.hpp"
#include "pcf8563.hpp"
#include "persistent_state.hpp"
#include "power_manager.hpp"
#include "sensor_suite.hpp"
#include "storage_partition.hpp"

namespace gathra {

class NodeApp final : public MaintenanceActions, public CommandEnvironment {
 public:
  NodeApp();
  void begin();
  void run();

  const NodeConfig& currentConfig() const override { return config_; }
  const Measurement& currentMeasurement() const override { return measurement_; }
  const TxReport& currentTxReport() const override { return lastTx_; }
  const PersistentState& persistentState() const override { return persistent_.state(); }
  uint16_t historyCount() const override { return history_.count(); }
  uint16_t historyCapacity() const override { return history_.capacity(); }
  uint32_t historyCorruptEntries() const override {
    return history_.corruptEntries();
  }
  bool historyAt(uint16_t index, HistoryEntry& entry) override {
    return history_.at(index, entry);
  }
  AppState currentAppState() const override { return state_; }
  protocol::BootReason bootReason() const override { return boot_.reason; }
  rtc::TimeState rtcTimeState() const override { return rtcTimeState_; }
  uint32_t rtcUnixTime() const override { return rtcUnixTime_; }
  const rtc::Status& pcfStatus() const override { return pcfStatus_; }
  uint32_t nextExpectedPollUnix() const override { return nextExpectedPollUnix_; }
  const char* maintenanceSource() const override { return maintenanceSource_; }
  uint32_t maintenanceRemainingSec() const override;
  const char* powerOffReadiness() const override { return powerOffReadiness_; }
  const char* macAddress() const override { return macAddress_; }
  const char* otaPartition() const override { return ota_.runningPartition(); }
  const char* otaImageState() const override { return ota_.imageStateName(); }
  const char* otaLastStatus() const override { return ota_.lastStatus(); }
  bool measureNow(String& error) override;
  bool applyConfiguration(const NodeConfig& candidate, String& error) override;
  bool captureCalibration(String& error) override;
  bool setCalibration(uint32_t referenceDistanceMm, String& error) override;
  TxReport sendRadioTest() override;
  bool prepareReboot(ExpectedRebootMode mode, String& error) override;
  void requestReboot() override { rebootRequested_ = true; }
  void requestMaintenanceExit() override { exitMaintenanceRequested_ = true; }

  // CommandEnvironment
  const StoredCommand& storedCommand() const override {
    return persistent_.state().command;
  }
  bool persistReceipt(const StoredCommand& command) override;
  protocol::CommandResultCode applyEnterMaintenance() override;
  protocol::CommandResultCode applyPollInterval(uint8_t minutes) override;
  protocol::CommandResultCode applyMaintenanceSchedule(uint32_t targetUnix) override;
  bool persistResult(const StoredCommand& command) override;
  uint8_t effectivePollIntervalMinutes() const override {
    return config_.pollIntervalMinutes;
  }
  uint32_t effectiveMaintenanceUnix() const override {
    return persistent_.state().scheduledMaintenanceUnix;
  }

 private:
  void acquireStage();
  void filterStage();
  void verifyStage();
  void applyFilterResult(const FilterResult& result);
  void transmitStage();
  protocol::TelemetryPacket makeTelemetry();
  bool appendCurrentHistory();
  bool refreshRtc();
  void synchronizeRtc(const protocol::AckCommandPacket& ack);
  void enterMaintenance(const char* source);
  void maintenanceLoop();
  bool finishMaintenance(ShutdownReason reason);
  bool finalPowerOff(ShutdownReason reason, bool maintenanceFinishBeep);
  void enterFault(const char* reason);
  void faultLoop();
  bool waitVerificationInterval(uint16_t intervalMs);
  void updateDerivedHeight();
  void prepareHealthAfterAcquisition();
  void buildIdentity();
  void beginWatchdog();
  void feedWatchdog();
  protocol::RtcState protocolRtcState() const;
  PowerEvent* currentPowerEvent();

  AppState state_ = AppState::kBoot;
  bool persistenceReady_ = false;
  bool historyReady_ = false;
  bool exitMaintenanceRequested_ = false;
  bool rebootRequested_ = false;
  bool historyPending_ = false;
  bool commandEnterMaintenance_ = false;
  uint8_t nextPollMinutes_ = 10;
  uint32_t maintenanceStartedMs_ = 0;
  uint32_t maintenanceLifetimeMs_ = 300000U;
  uint32_t lastRtcRefreshMs_ = 0;
  uint32_t nextExpectedPollUnix_ = 0;
  uint32_t faultRetryAtMs_ = 0;
  char macAddress_[18]{};
  char defaultNodeId_[build::kNodeIdCapacity]{};
  const char* maintenanceSource_ = "NONE";
  const char* powerOffReadiness_ = "NOT_STARTED";

  rtc::WireRegisterIo rtcIo_{};
  rtc::Pcf8563 rtc_;
  PowerManager power_;
  rtc::Status pcfStatus_{};
  rtc::TimeState rtcTimeState_ = rtc::TimeState::kI2cError;
  uint32_t rtcUnixTime_ = 0;
  BootClassification boot_{};

  NvsStateBackend stateBackend_{};
  PersistentStateManager persistent_{};
  TemporalFilter filter_;
  NvsHistoryBackend historyBackend_{};
  NvsHistory history_{};
  CommandProcessor commands_;
  NodeConfig config_{};
  ConfigStore configStore_{};
  SensorSuite sensors_{};
  LoraRadio radio_{};
  Buzzer buzzer_{};
  MaintenancePortal portal_{};
  OtaManager ota_{};
  Measurement measurement_{};
  FilterResult filterResult_{};
  TxReport lastTx_{};
};

}  // namespace gathra
