#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include "model.hpp"
#include "nvs_history.hpp"
#include "node_config.hpp"
#include "pcf8563.hpp"
#include "persistent_state.hpp"

namespace gathra {

class MaintenanceActions {
 public:
  virtual ~MaintenanceActions() = default;
  virtual const NodeConfig& currentConfig() const = 0;
  virtual const Measurement& currentMeasurement() const = 0;
  virtual const TxReport& currentTxReport() const = 0;
  virtual const PersistentState& persistentState() const = 0;
  virtual uint16_t historyCount() const = 0;
  virtual uint16_t historyCapacity() const = 0;
  virtual uint32_t historyCorruptEntries() const = 0;
  virtual bool historyAt(uint16_t index, HistoryEntry& entry) = 0;
  virtual AppState currentAppState() const = 0;
  virtual protocol::BootReason bootReason() const = 0;
  virtual rtc::TimeState rtcTimeState() const = 0;
  virtual uint32_t rtcUnixTime() const = 0;
  virtual const rtc::Status& pcfStatus() const = 0;
  virtual uint32_t nextExpectedPollUnix() const = 0;
  virtual const char* maintenanceSource() const = 0;
  virtual uint32_t maintenanceRemainingSec() const = 0;
  virtual const char* powerOffReadiness() const = 0;
  virtual const char* macAddress() const = 0;
  virtual const char* otaPartition() const = 0;
  virtual const char* otaImageState() const = 0;
  virtual const char* otaLastStatus() const = 0;
  virtual bool measureNow(String& error) = 0;
  virtual bool applyConfiguration(const NodeConfig& candidate, String& error) = 0;
  virtual bool captureCalibration(String& error) = 0;
  virtual bool setCalibration(uint32_t referenceDistanceMm, String& error) = 0;
  virtual TxReport sendRadioTest() = 0;
  virtual bool prepareReboot(ExpectedRebootMode mode, String& error) = 0;
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
  void sendDashboard();
  void sendStatus();
  void sendHistory();
  void sendHistoryChart();
  void sendConfig();
  void handleConfigUpdate();
  void sendMeasurementResult(const char* message);
  bool parseConfig(NodeConfig& candidate, String& error);
  bool sendBoundedJson(const String& output, uint32_t startedAtMs,
                       const char* endpoint);
  bool sendBoundedContent(const char* content, size_t length,
                          const char* contentType, uint32_t startedAtMs,
                          const char* endpoint);

  WebServer server_{80};
  MaintenanceActions* actions_ = nullptr;
  bool running_ = false;
  bool routesRegistered_ = false;
  uint32_t otaRebootAtMs_ = 0;
  uint32_t maintenanceExitAtMs_ = 0;
  bool otaUploadOk_ = false;
  char ssid_[33]{};
};

}  // namespace gathra
