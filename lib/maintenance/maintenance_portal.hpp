#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include "model.hpp"
#include "node_config.hpp"
#include "rtc_history.hpp"

namespace gathra {

class MaintenanceActions {
 public:
  virtual ~MaintenanceActions() = default;
  virtual const NodeConfig& currentConfig() const = 0;
  virtual const Measurement& currentMeasurement() const = 0;
  virtual const TxReport& currentTxReport() const = 0;
  virtual const RtcRetainedState& retainedState() const = 0;
  virtual AppState currentAppState() const = 0;
  virtual const char* macAddress() const = 0;
  virtual const char* otaPartition() const = 0;
  virtual const char* otaImageState() const = 0;
  virtual const char* otaLastStatus() const = 0;
  virtual bool measureNow(String& error) = 0;
  virtual bool applyConfiguration(const NodeConfig& candidate, String& error) = 0;
  virtual bool captureCalibration(String& error) = 0;
  virtual bool setCalibration(uint32_t referenceDistanceMm, String& error) = 0;
  virtual TxReport sendRadioTest() = 0;
  virtual void requestReboot() = 0;
  virtual void requestMaintenanceExit() = 0;
};

class MaintenancePortal {
 public:
  bool begin(MaintenanceActions& actions);
  void loop();
  void stop();
  bool running() const { return running_; }
  const char* ssid() const { return ssid_; }

 private:
  void registerRoutes();
  void noteActivity();
  void sendError(int status, const char* message);
  void sendStatus();
  void sendHistory();
  void sendConfig();
  void handleConfigUpdate();
  void sendMeasurementResult(const char* message);
  bool parseConfig(NodeConfig& candidate, String& error);

  WebServer server_{80};
  MaintenanceActions* actions_ = nullptr;
  bool running_ = false;
  bool routesRegistered_ = false;
  uint32_t lastActivityMs_ = 0;
  uint32_t otaRebootAtMs_ = 0;
  uint32_t maintenanceExitAtMs_ = 0;
  bool otaUploadOk_ = false;
  char ssid_[33]{};
};

}  // namespace gathra
