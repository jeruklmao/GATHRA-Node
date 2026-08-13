#pragma once

#include <Arduino.h>

#include "button.hpp"
#include "config_store.hpp"
#include "filter.hpp"
#include "lora_radio.hpp"
#include "maintenance_portal.hpp"
#include "model.hpp"
#include "ota_manager.hpp"
#include "rtc_history.hpp"
#include "sensor_suite.hpp"

namespace gathra {

class NodeApp final : public MaintenanceActions {
 public:
  NodeApp();
  void begin();
  void run();

  const NodeConfig& currentConfig() const override { return config_; }
  const Measurement& currentMeasurement() const override { return measurement_; }
  const TxReport& currentTxReport() const override { return lastTx_; }
  const RtcRetainedState& retainedState() const override;
  AppState currentAppState() const override { return state_; }
  const char* macAddress() const override { return macAddress_; }
  const char* otaPartition() const override { return ota_.runningPartition(); }
  const char* otaImageState() const override { return ota_.imageStateName(); }
  const char* otaLastStatus() const override { return ota_.lastStatus(); }
  bool measureNow(String& error) override;
  bool applyConfiguration(const NodeConfig& candidate, String& error) override;
  bool captureCalibration(String& error) override;
  bool setCalibration(uint32_t referenceDistanceMm, String& error) override;
  TxReport sendRadioTest() override;
  void requestReboot() override;
  void requestMaintenanceExit() override { exitMaintenanceRequested_ = true; }

 private:
  void acquireStage();
  void filterStage();
  void verifyStage(bool allowButtonTransition);
  void applyFilterResult(const FilterResult& result);
  void transmitStage();
  protocol::TelemetryPacket makeTelemetry() const;
  void appendCurrentHistory();
  void startMaintenance();
  bool waitVerificationInterval(uint16_t intervalMs, bool allowButtonTransition);
  void updateDerivedHeight();
  void prepareHealthAfterAcquisition();
  void buildIdentity();

  AppState state_ = AppState::kBoot;
  bool bootToMaintenance_ = false;
  bool maintenanceRequested_ = false;
  bool exitMaintenanceRequested_ = false;
  bool rebootRequested_ = false;
  bool historyPending_ = false;
  uint32_t sleepIntervalSec_ = 60;
  char macAddress_[18]{};
  char defaultNodeId_[build::kNodeIdCapacity]{};

  NodeConfig config_{};
  ConfigStore configStore_{};
  TemporalFilter filter_;
  SensorSuite sensors_{};
  LoraRadio radio_{};
  Button button_{};
  MaintenancePortal portal_{};
  OtaManager ota_{};
  Measurement measurement_{};
  FilterResult filterResult_{};
  TxReport lastTx_{};
};

}  // namespace gathra
