#include "maintenance_portal.hpp"

#include <ArduinoJson.h>
#include <Update.h>
#include <WiFi.h>
#include <errno.h>
#include <lwip/sockets.h>
#include <math.h>
#include <string.h>

#include "build_config.hpp"
#include "dashboard_html.hpp"
#include "esp32-hal.h"
#include "firmware_version.hpp"
#include "history_query.hpp"
#include "logger.hpp"

namespace gathra {
namespace {

constexpr uint32_t kHistoryRequestDeadlineMs = 2500U;
constexpr size_t kHistoryWriteChunkBytes = 512U;

bool parseUint16Argument(WebServer& server, const char* name,
                         uint16_t defaultValue, uint16_t& output) {
  if (!server.hasArg(name)) {
    output = defaultValue;
    return true;
  }
  const String text = server.arg(name);
  if (text.isEmpty()) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long value = strtoul(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0' || value > UINT16_MAX) {
    return false;
  }
  output = static_cast<uint16_t>(value);
  return true;
}

void addNullable(JsonObject object, const char* key, uint32_t value) {
  if (value == kDistanceUnavailable) object[key] = nullptr;
  else object[key] = value;
}

void addMeasurement(JsonObject object, const Measurement& m) {
  object["persistentSessionId"] = m.persistentSessionId;
  object["sequence"] = m.sequence;
  addNullable(object, "rawDistanceMm", m.sonar.medianDistanceMm);
  addNullable(object, "acceptedDistanceMm", m.acceptedDistanceMm);
  if (m.derivedWaterHeightMm == kHeightUnavailable) object["waterHeightMm"] = nullptr;
  else object["waterHeightMm"] = m.derivedWaterHeightMm;
  addNullable(object, "candidateDistanceMm", m.candidateDistanceMm);
  object["medianEchoUs"] = m.sonar.medianEchoUs;
  object["madMm"] = m.sonar.madMm;
  object["validSamples"] = m.sonar.validSamples;
  object["totalSamples"] = m.sonar.totalSamples;
  if (m.environment.valid) {
    object["temperatureC"] = m.environment.temperatureC;
    object["humidityPercent"] = m.environment.humidityPercent;
  } else {
    object["temperatureC"] = nullptr;
    object["humidityPercent"] = nullptr;
  }
  object["environmentValid"] = m.environment.valid;
  object["environmentAgeMs"] = m.environment.ageMs == UINT32_MAX ? 0U : m.environment.ageMs;
  object["batteryMv"] = m.battery.batteryMillivolts;
  object["batteryAdcRaw"] = m.battery.adcRaw;
  object["batteryAdcMv"] = m.battery.adcMillivolts;
  object["batteryAdcValid"] = m.battery.adcValid;
  object["filterState"] = filterStateName(m.filterState);
  object["healthFlags"] = m.healthFlags;
  object["qualityFlags"] = m.qualityFlags;
}

const char* commandPhaseName(CommandPhase phase) {
  switch (phase) {
    case CommandPhase::kNone: return "NONE";
    case CommandPhase::kReceived: return "RECEIVED";
    case CommandPhase::kCompleted: return "COMPLETED";
  }
  return "UNKNOWN";
}

template <typename T>
bool setIfPresent(JsonObjectConst object, const char* key, T& target, String& error) {
  JsonVariantConst value = object[key];
  if (value.isNull()) return true;
  if (!value.is<T>()) {
    error = "invalid type or numeric range for ";
    error += key;
    return false;
  }
  target = value.as<T>();
  return true;
}

}  // namespace

bool MaintenancePortal::begin(MaintenanceActions& actions) {
  actions_ = &actions;
  const char* id = actions.currentConfig().nodeId;
  const size_t length = strlen(id);
  const char* suffix = length > 18U ? id + length - 18U : id;
  snprintf(ssid_, sizeof(ssid_), "GATHRA-NODE-%s", suffix);
  WiFi.mode(WIFI_AP);
  (void)WiFi.setSleep(false);
  delay(100);
  if (!WiFi.softAP(ssid_, build::kMaintenancePassword, 1, false, 4)) {
    GTH_LOGE("WEB", "SoftAP start failed");
    return false;
  }
  if (!routesRegistered_) {
    registerRoutes();
    routesRegistered_ = true;
  }
  server_.begin();
  running_ = true;
  GTH_LOGI("WEB", "maintenance AP SSID=%s IP=%s channel=1 WPA2=yes",
           ssid_, WiFi.softAPIP().toString().c_str());
  return true;
}

void MaintenancePortal::noteActivity() {
  // Passive use never extends NodeApp's fixed maintenance energy deadline.
}

void MaintenancePortal::loop() {
  if (!running_ || actions_ == nullptr) return;
  server_.handleClient();
  if (otaRebootAtMs_ != 0U && static_cast<int32_t>(millis() - otaRebootAtMs_) >= 0) {
    actions_->requestReboot();
    otaRebootAtMs_ = 0U;
  }
  if (maintenanceExitAtMs_ != 0U &&
      static_cast<int32_t>(millis() - maintenanceExitAtMs_) >= 0) {
    actions_->requestMaintenanceExit();
    maintenanceExitAtMs_ = 0U;
  }
}

void MaintenancePortal::stop() {
  if (!running_) return;
  server_.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  running_ = false;
  GTH_LOGI("WEB", "maintenance AP stopped");
}

void MaintenancePortal::sendError(int status, const char* message) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = message;
  String output;
  serializeJson(doc, output);
  server_.send(status, "application/json", output);
}

void MaintenancePortal::registerRoutes() {
  server_.on("/", HTTP_GET, [this]() {
    noteActivity();
    sendDashboard();
  });
  server_.on("/api/status", HTTP_GET, [this]() { sendStatus(); });
  server_.on("/api/history", HTTP_GET, [this]() { sendHistory(); });
  server_.on("/api/history/chart", HTTP_GET, [this]() { sendHistoryChart(); });
  server_.on("/api/config", HTTP_GET, [this]() { sendConfig(); });
  server_.on("/api/config", HTTP_PUT, [this]() { handleConfigUpdate(); });
  server_.on("/api/measure", HTTP_POST, [this]() {
    String error;
    if (!actions_->measureNow(error)) return sendError(503, error.c_str());
    sendMeasurementResult("measurement completed and persisted through production pipeline");
  });
  server_.on("/api/calibration/capture", HTTP_POST, [this]() {
    String error;
    if (!actions_->captureCalibration(error)) return sendError(422, error.c_str());
    sendMeasurementResult("current stable accepted distance captured as reference");
  });
  server_.on("/api/calibration/set", HTTP_POST, [this]() {
    JsonDocument doc;
    if (deserializeJson(doc, server_.arg("plain"))) return sendError(400, "invalid JSON");
    const uint32_t reference = doc["referenceDistanceMm"] | UINT32_MAX;
    String error;
    if (!actions_->setCalibration(reference, error)) return sendError(422, error.c_str());
    sendMeasurementResult(reference == 0U ? "reference cleared" : "reference saved");
  });
  server_.on("/api/radio/test", HTTP_POST, [this]() {
    const TxReport report = actions_->sendRadioTest();
    JsonDocument doc;
    doc["ok"] = report.transmitted;
    doc["message"] = report.acknowledged ? "matching Protocol v3 ACK received"
                                          : "bounded radio test completed without ACK";
    doc["radioReady"] = report.radioReady;
    doc["transmitted"] = report.transmitted;
    doc["acknowledged"] = report.acknowledged;
    doc["attempts"] = report.attempts;
    doc["radioCode"] = report.lastRadioCode;
    if (isfinite(report.lastRssi)) doc["rssi"] = report.lastRssi;
    else doc["rssi"] = nullptr;
    if (isfinite(report.lastSnr)) doc["snr"] = report.lastSnr;
    else doc["snr"] = nullptr;
    String output;
    serializeJson(doc, output);
    server_.send(200, "application/json", output);
  });
  server_.on("/api/logs", HTTP_GET, [this]() {
    JsonDocument doc;
    JsonArray lines = doc["lines"].to<JsonArray>();
    Logger& logger = Logger::instance();
    char line[160]{};
    for (uint8_t i = 0; i < logger.count(); ++i) {
      if (logger.at(i, line, sizeof(line))) lines.add(line);
    }
    String output;
    serializeJson(doc, output);
    server_.send(200, "application/json", output);
  });
  server_.on("/api/activity", HTTP_POST, [this]() {
    noteActivity();
    server_.send(204, "text/plain", "");
  });
  server_.on("/api/reboot", HTTP_POST, [this]() {
    String error;
    if (!actions_->prepareReboot(ExpectedRebootMode::kMaintenance, error))
      return sendError(409, error.c_str());
    server_.send(200, "application/json",
                 "{\"ok\":true,\"message\":\"maintenance reboot marker persisted; reboot scheduled\"}");
    otaRebootAtMs_ = millis() + 500U;
  });
  server_.on("/api/maintenance/exit", HTTP_POST, [this]() {
    server_.send(200, "application/json",
                 "{\"ok\":true,\"message\":\"verified hard-power shutdown scheduled\"}");
    maintenanceExitAtMs_ = millis() + 500U;
  });
  server_.on(
      "/api/ota", HTTP_POST,
      [this]() {
        if (!otaUploadOk_ || Update.hasError()) {
          sendError(500, Update.errorString());
          return;
        }
        String error;
        if (!actions_->prepareReboot(ExpectedRebootMode::kOta, error)) {
          sendError(409, error.c_str());
          return;
        }
        server_.send(200, "application/json",
                     "{\"ok\":true,\"message\":\"OTA accepted; persistent OTA marker and RTC latch verified\"}");
        otaRebootAtMs_ = millis() + 800U;
      },
      [this]() {
        // WebServer streams the complete firmware from this loop-task handler.
        // Keep watchdog protection enabled while explicitly reporting progress
        // once per bounded upload chunk.
        feedLoopWDT();
        HTTPUpload& upload = server_.upload();
        if (upload.status == UPLOAD_FILE_START) {
          otaUploadOk_ = Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
          GTH_LOGI("OTA", "upload start file=%s", upload.filename.c_str());
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (otaUploadOk_ &&
              Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            otaUploadOk_ = false;
          }
          feedLoopWDT();
        } else if (upload.status == UPLOAD_FILE_END) {
          otaUploadOk_ = otaUploadOk_ && Update.end(true);
          GTH_LOGI("OTA", "upload end bytes=%u ok=%s", upload.totalSize,
                   otaUploadOk_ ? "yes" : "no");
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
          Update.abort();
          otaUploadOk_ = false;
          GTH_LOGW("OTA", "upload aborted");
        }
      });
  server_.onNotFound([this]() { sendError(404, "not found"); });
}

void MaintenancePortal::sendStatus() {
  const Measurement& measurementState = actions_->currentMeasurement();
  const TxReport& tx = actions_->currentTxReport();
  const PersistentState& state = actions_->persistentState();
  const rtc::Status& pcf = actions_->pcfStatus();
  JsonDocument doc;
  doc["firmwareVersion"] = GATHRA_FIRMWARE_VERSION;
  doc["protocolVersion"] = GATHRA_PROTOCOL_VERSION;
  doc["build"] = GATHRA_BUILD_ID;
  doc["mac"] = actions_->macAddress();
  doc["nodeId"] = actions_->currentConfig().nodeId;
  doc["uptimeMs"] = millis();
  doc["state"] = appStateName(actions_->currentAppState());
  doc["bootReason"] = protocol::bootReasonName(actions_->bootReason());
  doc["configSchemaVersion"] = actions_->currentConfig().schemaVersion;

  JsonObject maintenance = doc["maintenance"].to<JsonObject>();
  maintenance["source"] = actions_->maintenanceSource();
  maintenance["remainingSeconds"] = actions_->maintenanceRemainingSec();
  maintenance["fixedMaximumSeconds"] = build::kMaintenanceMaximumSec;
  maintenance["persistedActive"] = state.maintenanceActive;
  maintenance["persistedDeadlineUtc"] = state.maintenanceDeadlineUnix;

  JsonObject pcfObject = doc["pcf8563"].to<JsonObject>();
  pcfObject["communicationOkay"] = pcf.communicationOkay;
  pcfObject["controlStatus1"] = pcf.control1;
  pcfObject["controlStatus2"] = pcf.control2;
  pcfObject["timerFlag"] = pcf.timerFlag;
  pcfObject["alarmFlag"] = pcf.alarmFlag;
  pcfObject["timerInterruptEnabled"] = pcf.timerInterruptEnabled;
  pcfObject["alarmInterruptEnabled"] = pcf.alarmInterruptEnabled;
  pcfObject["levelMode"] = pcf.levelMode;

  JsonObject rtcObject = doc["rtc"].to<JsonObject>();
  rtcObject["state"] = rtc::timeStateName(actions_->rtcTimeState());
  if (actions_->rtcTimeState() == rtc::TimeState::kValid)
    rtcObject["utcUnix"] = actions_->rtcUnixTime();
  else
    rtcObject["utcUnix"] = nullptr;
  rtcObject["lastGatewayUtc"] = state.lastGatewayUtc;
  rtcObject["lastSuccessfulSyncUtc"] = state.lastRtcSyncUnix;

  JsonObject power = doc["power"].to<JsonObject>();
  power["pollIntervalMinutes"] = actions_->currentConfig().pollIntervalMinutes;
  power["nextExpectedPollUtc"] = actions_->nextExpectedPollUnix();
  power["scheduleState"] = protocol::scheduleStateName(state.scheduleState);
  power["scheduledMaintenanceUtc"] = state.scheduledMaintenanceUnix;
  power["powerOffReadiness"] = actions_->powerOffReadiness();
  power["expectedReboot"] = expectedRebootName(state.expectedReboot);

  JsonObject protocolState = doc["protocol"].to<JsonObject>();
  protocolState["persistentSessionId"] = state.persistentSessionId;
  protocolState["nextSequence"] = state.nextSequence;

  JsonObject command = doc["lastCommand"].to<JsonObject>();
  command["commandId"] = state.command.commandId;
  command["type"] = protocol::commandTypeName(state.command.commandType);
  command["phase"] = commandPhaseName(state.command.phase);
  command["result"] = protocol::commandResultName(state.command.result);
  command["effectivePollIntervalMinutes"] = state.command.effectivePollIntervalMinutes;
  command["scheduledMaintenanceUtc"] = state.command.effectiveMaintenanceUnix;

  JsonObject currentMeasurement = doc["measurement"].to<JsonObject>();
  addMeasurement(currentMeasurement, measurementState);
  JsonObject radio = doc["radio"].to<JsonObject>();
  radio["ready"] = tx.radioReady;
  radio["transmitted"] = tx.transmitted;
  radio["acknowledged"] = tx.acknowledged;
  radio["attempts"] = tx.attempts;
  radio["lastCode"] = tx.lastRadioCode;
  radio["commandResultTransmitted"] = tx.commandResultTransmitted;

  JsonObject history = doc["history"].to<JsonObject>();
  history["storage"] = "NVS circular slots";
  history["count"] = actions_->historyCount();
  history["capacity"] = actions_->historyCapacity();
  history["entryBytes"] = sizeof(HistoryRecord);
  history["corruptEntriesSkipped"] = actions_->historyCorruptEntries();

  JsonObject candidate = doc["candidate"].to<JsonObject>();
  addNullable(candidate, "distanceMm", state.filter.lastCandidateMm);
  candidate["observations"] = state.filter.lastCandidateObservations;
  candidate["confirmations"] = state.filter.lastCandidateVotes;
  if (state.filter.hasAccepted) candidate["lastAcceptedMm"] = state.filter.lastAcceptedMm;
  else candidate["lastAcceptedMm"] = nullptr;
  candidate["emaDistanceMm"] = state.filter.emaDistanceMm;

  JsonObject calibration = doc["calibration"].to<JsonObject>();
  calibration["configured"] = actions_->currentConfig().referenceDistanceMm != 0U;
  calibration["referenceDistanceMm"] = actions_->currentConfig().referenceDistanceMm;

  JsonArray events = doc["powerEvents"].to<JsonArray>();
  const uint8_t oldest = static_cast<uint8_t>(
      (state.powerEventHead + 8U - state.powerEventCount) % 8U);
  for (uint8_t i = 0; i < state.powerEventCount; ++i) {
    const PowerEvent& event = state.powerEvents[(oldest + i) % 8U];
    JsonObject value = events.add<JsonObject>();
    value["counter"] = event.eventCounter;
    value["bootReason"] = protocol::bootReasonName(event.bootReason);
    value["pcfFlagsAtBoot"] = event.pcfStatusAtBoot;
    value["rtcUtcAtBoot"] = event.rtcUnixAtBoot;
    value["shutdownReason"] = shutdownReasonName(event.shutdownReason);
    value["timerMinutes"] = event.timerMinutes;
    value["scheduledMaintenanceUtc"] = event.scheduledMaintenanceUnix;
    value["flagClearAttempted"] = event.flagClearAttempted;
    value["flagClearSucceeded"] = event.flagClearSucceeded;
    value["commandId"] = event.commandId;
  }

  JsonObject ota = doc["ota"].to<JsonObject>();
  ota["partition"] = actions_->otaPartition();
  ota["imageState"] = actions_->otaImageState();
  ota["lastStatus"] = actions_->otaLastStatus();
  String output;
  serializeJson(doc, output);
  server_.send(200, "application/json", output);
}

void MaintenancePortal::sendHistory() {
  const uint32_t startedAtMs = millis();
  const uint32_t heapBefore = ESP.getFreeHeap();
  uint16_t offset = 0U;
  uint16_t limit = history_query::kDefaultPageSize;
  if (!parseUint16Argument(server_, "offset", 0U, offset) ||
      !parseUint16Argument(server_, "limit", history_query::kDefaultPageSize,
                           limit)) {
    return sendError(400, "offset and limit must be unsigned 16-bit integers");
  }
  const history_query::Page page =
      history_query::page(actions_->historyCount(), offset, limit);
  const uint32_t reference = actions_->currentConfig().referenceDistanceMm;
  JsonDocument doc;
  doc["count"] = page.total;
  doc["capacity"] = actions_->historyCapacity();
  doc["offset"] = page.offset;
  doc["limit"] = page.limit;
  doc["returned"] = page.returned;
  doc["hasPrevious"] = page.hasPrevious;
  doc["hasNext"] = page.hasNext;
  if (page.hasNext) doc["nextOffset"] = page.nextOffset;
  else doc["nextOffset"] = nullptr;
  doc["timeBasis"] = "PCF8563 UTC when trusted; null means unavailable";
  JsonArray entries = doc["entries"].to<JsonArray>();
  HistoryEntry entry{};
  for (uint16_t i = page.offset; i < page.nextOffset; ++i) {
    if (!actions_->historyAt(i, entry)) continue;
    JsonObject value = entries.add<JsonObject>();
    value["sequence"] = entry.sequence;
    if (entry.rtcUnixTime == 0U) value["rtcUtc"] = nullptr;
    else value["rtcUtc"] = entry.rtcUnixTime;
    if (entry.rawDistanceMm == kDistanceUnavailable) value["rawDistanceMm"] = nullptr;
    else value["rawDistanceMm"] = entry.rawDistanceMm;
    if (entry.acceptedDistanceMm == kDistanceUnavailable) {
      value["acceptedDistanceMm"] = nullptr;
      value["waterHeightMm"] = nullptr;
    } else {
      value["acceptedDistanceMm"] = entry.acceptedDistanceMm;
      if (reference == 0U) value["waterHeightMm"] = nullptr;
      else value["waterHeightMm"] = static_cast<int32_t>(reference) -
                                      static_cast<int32_t>(entry.acceptedDistanceMm);
    }
    value["madMm"] = entry.madMm;
    value["batteryMv"] = entry.batteryMv;
    if (entry.temperatureCentiC == kTemperatureUnavailable) value["temperatureC"] = nullptr;
    else value["temperatureC"] = static_cast<float>(entry.temperatureCentiC) / 100.0F;
    if (entry.humidityCentiPercent == kHumidityUnavailable) value["humidityPercent"] = nullptr;
    else value["humidityPercent"] = static_cast<float>(entry.humidityCentiPercent) / 100.0F;
    value["filterState"] = filterStateName(static_cast<FilterState>(entry.filterState));
    value["qualityFlags"] = entry.qualityFlags;
    value["healthFlags"] = entry.healthFlags;
    value["validSamples"] = entry.validSamples;
    value["totalSamples"] = entry.totalSamples;
  }
  String output;
  serializeJson(doc, output);
  GTH_LOGI("WEB", "history page offset=%u returned=%u/%u bytes=%u buildMs=%lu heap=%lu->%lu min=%lu",
           page.offset, page.returned, page.total, output.length(),
           static_cast<unsigned long>(millis() - startedAtMs),
           static_cast<unsigned long>(heapBefore),
           static_cast<unsigned long>(ESP.getFreeHeap()),
           static_cast<unsigned long>(ESP.getMinFreeHeap()));
  (void)sendBoundedJson(output, startedAtMs, "/api/history");
}

void MaintenancePortal::sendDashboard() {
  const uint32_t startedAtMs = millis();
  (void)sendBoundedContent(dashboard::kHtml, sizeof(dashboard::kHtml) - 1U,
                           "text/html; charset=utf-8", startedAtMs, "/");
}

void MaintenancePortal::sendHistoryChart() {
  const uint32_t startedAtMs = millis();
  const uint32_t heapBefore = ESP.getFreeHeap();
  uint16_t requestedPoints = history_query::kDefaultChartPoints;
  if (!parseUint16Argument(server_, "maxPoints", history_query::kDefaultChartPoints,
                           requestedPoints)) {
    return sendError(400, "maxPoints must be an unsigned 16-bit integer");
  }
  const String series = server_.hasArg("series")
      ? server_.arg("series") : String("rawDistanceMm");
  const bool validSeries = series == "rawDistanceMm" ||
      series == "acceptedDistanceMm" || series == "waterHeightMm" ||
      series == "temperatureC" || series == "humidityPercent" ||
      series == "batteryMv";
  if (!validSeries) return sendError(400, "unsupported chart series");

  const uint16_t total = actions_->historyCount();
  const uint16_t points = history_query::chartPointCount(total, requestedPoints);
  const uint32_t reference = actions_->currentConfig().referenceDistanceMm;
  JsonDocument doc;
  doc["count"] = total;
  doc["maxPoints"] = points;
  doc["series"] = series;
  JsonArray entries = doc["entries"].to<JsonArray>();
  HistoryEntry entry{};
  for (uint16_t point = 0U; point < points; ++point) {
    const uint16_t index = history_query::chartIndex(total, points, point);
    if (!actions_->historyAt(index, entry)) continue;
    JsonObject value = entries.add<JsonObject>();
    // Compact chart keys keep a 100-point response comfortably below the
    // ESP32-C3 socket-pressure threshold observed with the legacy endpoint.
    value["s"] = entry.sequence;
    value["f"] = entry.filterState;
    if (series == "rawDistanceMm") {
      if (entry.rawDistanceMm == kDistanceUnavailable) value["v"] = nullptr;
      else value["v"] = entry.rawDistanceMm;
    } else if (series == "acceptedDistanceMm") {
      if (entry.acceptedDistanceMm == kDistanceUnavailable) value["v"] = nullptr;
      else value["v"] = entry.acceptedDistanceMm;
    } else if (series == "waterHeightMm") {
      if (reference == 0U || entry.acceptedDistanceMm == kDistanceUnavailable) {
        value["v"] = nullptr;
      } else {
        value["v"] = static_cast<int32_t>(reference) -
                      static_cast<int32_t>(entry.acceptedDistanceMm);
      }
    } else if (series == "temperatureC") {
      if (entry.temperatureCentiC == kTemperatureUnavailable) value["v"] = nullptr;
      else value["v"] = static_cast<float>(entry.temperatureCentiC) / 100.0F;
    } else if (series == "humidityPercent") {
      if (entry.humidityCentiPercent == kHumidityUnavailable) value["v"] = nullptr;
      else value["v"] = static_cast<float>(entry.humidityCentiPercent) / 100.0F;
    } else {
      value["v"] = entry.batteryMv;
    }
  }
  String output;
  serializeJson(doc, output);
  GTH_LOGI("WEB", "history chart series=%s points=%u/%u bytes=%u buildMs=%lu heap=%lu->%lu min=%lu",
           series.c_str(), points, total, output.length(),
           static_cast<unsigned long>(millis() - startedAtMs),
           static_cast<unsigned long>(heapBefore),
           static_cast<unsigned long>(ESP.getFreeHeap()),
           static_cast<unsigned long>(ESP.getMinFreeHeap()));
  (void)sendBoundedJson(output, startedAtMs, "/api/history/chart");
}

bool MaintenancePortal::sendBoundedJson(const String& output,
                                        uint32_t startedAtMs,
                                        const char* endpoint) {
  return sendBoundedContent(output.c_str(), output.length(),
                            "application/json", startedAtMs, endpoint);
}

bool MaintenancePortal::sendBoundedContent(const char* content, size_t length,
                                           const char* contentType,
                                           uint32_t startedAtMs,
                                           const char* endpoint) {
  WiFiClient client = server_.client();
  const int socket = client.fd();
  if (socket < 0 || !client.connected()) return false;

  server_.setContentLength(length);
  // Let WebServer produce its normal headers, but give it only one safe body
  // byte. The remaining content uses the bounded nonblocking path below.
  const String firstByte = length == 0U ? String() : String(content[0]);
  server_.send(200, contentType, firstByte);
  size_t sentTotal = length == 0U ? 0U : 1U;
  while (sentTotal < length) {
    if (!client.connected() ||
        millis() - startedAtMs >= kHistoryRequestDeadlineMs) {
      GTH_LOGW("WEB", "%s aborted bytes=%u/%u elapsedMs=%lu reason=%s",
               endpoint, sentTotal, length,
               static_cast<unsigned long>(millis() - startedAtMs),
               client.connected() ? "deadline" : "disconnect");
      client.stop();
      return false;
    }

    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socket, &writable);
    timeval wait{0, 20000};
    const int ready = select(socket + 1, nullptr, &writable, nullptr, &wait);
    if (ready < 0) {
      GTH_LOGW("WEB", "%s select failed errno=%d", endpoint, errno);
      client.stop();
      return false;
    }
    if (ready == 0 || !FD_ISSET(socket, &writable)) {
      delay(1);
      continue;
    }

    const size_t remaining = length - sentTotal;
    const size_t chunk = remaining < kHistoryWriteChunkBytes
        ? remaining : kHistoryWriteChunkBytes;
    const int written = ::send(socket, content + sentTotal, chunk, MSG_DONTWAIT);
    if (written > 0) {
      sentTotal += static_cast<size_t>(written);
      delay(0);
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      delay(1);
      continue;
    }
    GTH_LOGW("WEB", "%s write failed bytes=%u/%u errno=%d",
             endpoint, sentTotal, length, errno);
    client.stop();
    return false;
  }
  GTH_LOGI("WEB", "%s sent bytes=%u elapsedMs=%lu", endpoint, sentTotal,
           static_cast<unsigned long>(millis() - startedAtMs));
  return true;
}

void MaintenancePortal::sendConfig() {
  const NodeConfig& c = actions_->currentConfig();
  JsonDocument doc;
#define CONFIG_VALUE(name) doc[#name] = c.name
  CONFIG_VALUE(schemaVersion); CONFIG_VALUE(nodeId); CONFIG_VALUE(pollIntervalMinutes);
  CONFIG_VALUE(sonarBurstCount); CONFIG_VALUE(sonarMinimumValid); CONFIG_VALUE(sonarInterPingMs);
  CONFIG_VALUE(hampelWindow); CONFIG_VALUE(hampelMultiplier); CONFIG_VALUE(hampelAbsoluteFloorMm);
  CONFIG_VALUE(suddenChangeThresholdMm); CONFIG_VALUE(riseVerificationCount);
  CONFIG_VALUE(riseRequiredConfirmations); CONFIG_VALUE(riseVerificationIntervalMs);
  CONFIG_VALUE(riseToleranceMm); CONFIG_VALUE(fallVerificationCount);
  CONFIG_VALUE(fallRequiredConfirmations); CONFIG_VALUE(fallVerificationIntervalMs);
  CONFIG_VALUE(fallToleranceMm); CONFIG_VALUE(emaAlpha); CONFIG_VALUE(referenceDistanceMm);
  CONFIG_VALUE(installationMinimumDistanceMm); CONFIG_VALUE(installationMaximumDistanceMm);
  CONFIG_VALUE(batteryCalibrationFactor); CONFIG_VALUE(batteryCalibrationOffsetMv);
  CONFIG_VALUE(batteryLowMv); CONFIG_VALUE(batteryCriticalMv); CONFIG_VALUE(loraFrequencyMhz);
  CONFIG_VALUE(loraBandwidthKhz); CONFIG_VALUE(loraSpreadingFactor);
  CONFIG_VALUE(loraCodingRateDenominator); CONFIG_VALUE(loraTxPowerDbm);
  CONFIG_VALUE(loraSyncWord); CONFIG_VALUE(ackTimeoutMs); CONFIG_VALUE(ackRetryCount);
  CONFIG_VALUE(maintenanceTimeoutSec);
#undef CONFIG_VALUE
  String output;
  serializeJson(doc, output);
  server_.send(200, "application/json", output);
}

bool MaintenancePortal::parseConfig(NodeConfig& c, String& error) {
  JsonDocument doc;
  const DeserializationError jsonError = deserializeJson(doc, server_.arg("plain"));
  if (jsonError) {
    error = "invalid JSON: ";
    error += jsonError.c_str();
    return false;
  }
  if (!doc.is<JsonObject>()) {
    error = "configuration payload must be a JSON object";
    return false;
  }
  JsonObjectConst object = doc.as<JsonObjectConst>();
  if (!object["nodeId"].isNull()) {
    if (!object["nodeId"].is<const char*>()) {
      error = "nodeId must be a string";
      return false;
    }
    const char* nodeId = object["nodeId"].as<const char*>();
    strncpy(c.nodeId, nodeId, sizeof(c.nodeId) - 1U);
    c.nodeId[sizeof(c.nodeId) - 1U] = '\0';
  }
#define SET_CONFIG(name, type)                                           \
  do {                                                                   \
    if (!setIfPresent<type>(object, #name, c.name, error)) return false; \
  } while (false)
  SET_CONFIG(pollIntervalMinutes, uint8_t);
  SET_CONFIG(sonarBurstCount, uint8_t); SET_CONFIG(sonarMinimumValid, uint8_t);
  SET_CONFIG(sonarInterPingMs, uint16_t); SET_CONFIG(hampelWindow, uint8_t);
  SET_CONFIG(hampelMultiplier, float); SET_CONFIG(hampelAbsoluteFloorMm, uint16_t);
  SET_CONFIG(suddenChangeThresholdMm, uint16_t); SET_CONFIG(riseVerificationCount, uint8_t);
  SET_CONFIG(riseRequiredConfirmations, uint8_t); SET_CONFIG(riseVerificationIntervalMs, uint16_t);
  SET_CONFIG(riseToleranceMm, uint16_t); SET_CONFIG(fallVerificationCount, uint8_t);
  SET_CONFIG(fallRequiredConfirmations, uint8_t); SET_CONFIG(fallVerificationIntervalMs, uint16_t);
  SET_CONFIG(fallToleranceMm, uint16_t); SET_CONFIG(emaAlpha, float);
  SET_CONFIG(referenceDistanceMm, uint32_t); SET_CONFIG(installationMinimumDistanceMm, uint32_t);
  SET_CONFIG(installationMaximumDistanceMm, uint32_t); SET_CONFIG(batteryCalibrationFactor, float);
  SET_CONFIG(batteryCalibrationOffsetMv, int16_t); SET_CONFIG(batteryLowMv, uint16_t);
  SET_CONFIG(batteryCriticalMv, uint16_t); SET_CONFIG(loraFrequencyMhz, float);
  SET_CONFIG(loraBandwidthKhz, float); SET_CONFIG(loraSpreadingFactor, uint8_t);
  SET_CONFIG(loraCodingRateDenominator, uint8_t); SET_CONFIG(loraTxPowerDbm, int8_t);
  SET_CONFIG(loraSyncWord, uint8_t); SET_CONFIG(ackTimeoutMs, uint16_t);
  SET_CONFIG(ackRetryCount, uint8_t); SET_CONFIG(maintenanceTimeoutSec, uint16_t);
#undef SET_CONFIG
  const ConfigValidationResult validation = validateConfig(c);
  if (!validation) {
    error = validation.message;
    return false;
  }
  return true;
}

void MaintenancePortal::handleConfigUpdate() {
  NodeConfig candidate = actions_->currentConfig();
  String error;
  if (!parseConfig(candidate, error)) return sendError(422, error.c_str());
  if (!actions_->applyConfiguration(candidate, error)) return sendError(409, error.c_str());
  server_.send(200, "application/json",
               "{\"ok\":true,\"message\":\"configuration validated, applied and persisted\"}");
}

void MaintenancePortal::sendMeasurementResult(const char* message) {
  JsonDocument doc;
  doc["ok"] = true;
  doc["message"] = message;
  JsonObject measurement = doc["measurement"].to<JsonObject>();
  addMeasurement(measurement, actions_->currentMeasurement());
  String output;
  serializeJson(doc, output);
  server_.send(200, "application/json", output);
}

}  // namespace gathra
