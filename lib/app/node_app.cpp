#include "node_app.hpp"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "build_config.hpp"
#include "esp_mac.h"
#include "esp_system.h"
#include "firmware_version.hpp"
#include "logger.hpp"
#include "power_manager.hpp"

namespace {
RTC_DATA_ATTR gathra::RtcRetainedState gRtcState;
}

namespace gathra {
namespace {

int16_t toCentiTemperature(float value) {
  if (!isfinite(value) || value < -327.67F || value > 327.67F) return kTemperatureUnavailable;
  return static_cast<int16_t>(lroundf(value * 100.0F));
}

uint16_t toCentiHumidity(float value) {
  if (!isfinite(value) || value < 0.0F || value > 100.0F) return kHumidityUnavailable;
  return static_cast<uint16_t>(lroundf(value * 100.0F));
}

}  // namespace

NodeApp::NodeApp() : filter_(gRtcState.filter) {}

const RtcRetainedState& NodeApp::retainedState() const { return gRtcState; }

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
  Serial.begin(build::kSerialBaud);
  delay(350);
  Logger::instance().begin();
  buildIdentity();
  GTH_LOGI("APP", "GATHRA Node firmware=%s build=%s", GATHRA_FIRMWARE_VERSION,
           GATHRA_BUILD_ID);
  GTH_LOGI("APP", "MAC=%s reset=%s wake=%s", macAddress_,
           PowerManager::resetReasonName(), PowerManager::wakeReasonName());

  const bool loaded = configStore_.load(config_, defaultNodeId_);
  GTH_LOGI("CONFIG", "%s", configStore_.lastError());
  if (!loaded) GTH_LOGW("CONFIG", "safe defaults active");

  const bool retainedWake = esp_reset_reason() == ESP_RST_DEEPSLEEP;
  if (!retainedWake || !rtcStateValid(gRtcState)) {
    uint32_t session = esp_random();
    if (session == 0U) session = 1U;
    initializeRtcState(gRtcState, session);
    GTH_LOGI("RTC", "new retained session id=%08lX size=%u bytes",
             static_cast<unsigned long>(session), static_cast<unsigned>(sizeof(gRtcState)));
  } else {
    GTH_LOGI("RTC", "retained session restored id=%08lX sequence=%lu history=%u/%u",
             static_cast<unsigned long>(gRtcState.bootSessionId),
             static_cast<unsigned long>(gRtcState.sequenceCounter), gRtcState.historyCount,
             build::kHistoryCapacity);
  }

  ota_.begin();
  button_.begin();
  const bool sonarReady = sensors_.begin();
  const bool radioReady = radio_.begin(config_);
  lastTx_.radioReady = radioReady;
  const bool internalStateSane = static_cast<bool>(validateConfig(config_)) &&
                                 rtcStateValid(gRtcState);
  if (!ota_.completeBootValidation(internalStateSane)) {
    GTH_LOGE("APP", "boot validation did not complete cleanly");
  }
  const bool otaMaintenance = OtaManager::takeMaintenanceAfterRebootRequest();
  if (otaMaintenance) {
    GTH_LOGI("OTA", "consumed one-shot request to return to maintenance after reboot");
  }

  GTH_LOGI("APP", "peripherals sonar=%s radio=%s (degradation does not invalidate OTA)",
           sonarReady ? "ready" : "degraded", radioReady ? "ready" : "degraded");
  bootToMaintenance_ = PowerManager::wokeFromButton() || button_.isPressed() || otaMaintenance;
#ifdef GATHRA_FORCE_MAINTENANCE
  bootToMaintenance_ = true;
  GTH_LOGW("APP", "HIL profile forces maintenance mode");
#endif
  sleepIntervalSec_ = config_.normalWakeIntervalSec;
  state_ = AppState::kBoot;
}

void NodeApp::run() {
  button_.update();
  if (state_ != AppState::kMaintenance && button_.takePressedEvent()) {
    maintenanceRequested_ = true;
    GTH_LOGI("BUTTON", "debounced press requests maintenance at next safe boundary");
  }

  switch (state_) {
    case AppState::kBoot:
      state_ = (bootToMaintenance_ || maintenanceRequested_) ? AppState::kMaintenance
                                                             : AppState::kAcquire;
      break;

    case AppState::kAcquire:
      if (maintenanceRequested_) {
        state_ = AppState::kMaintenance;
        break;
      }
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
      verifyStage(true);
      if (maintenanceRequested_) {
        appendCurrentHistory();
        state_ = AppState::kMaintenance;
      } else {
        state_ = AppState::kTransmit;
      }
      break;

    case AppState::kTransmit:
      transmitStage();
      appendCurrentHistory();
      state_ = maintenanceRequested_ ? AppState::kMaintenance : AppState::kSleep;
      break;

    case AppState::kMaintenance:
      startMaintenance();
      portal_.loop();
      button_.update();
      if (button_.longPressed(build::kMaintenanceLongPressMs)) {
        GTH_LOGI("BUTTON", "maintenance long press requests sleep");
        exitMaintenanceRequested_ = true;
      }
      if (rebootRequested_) {
        portal_.stop();
        radio_.sleep();
        GTH_LOGI("APP", "restarting");
        Serial.flush();
        delay(150);
        ESP.restart();
      }
      if (exitMaintenanceRequested_) {
        portal_.stop();
        radio_.sleep();
        maintenanceRequested_ = false;
        exitMaintenanceRequested_ = false;
        sleepIntervalSec_ = config_.normalWakeIntervalSec;
        state_ = AppState::kSleep;
      }
      delay(2);
      break;

    case AppState::kSleep:
      if (maintenanceRequested_) {
        state_ = AppState::kMaintenance;
        break;
      }
      portal_.stop();
      radio_.sleep();
      PowerManager::deepSleep(sleepIntervalSec_);
      break;
  }
}

void NodeApp::acquireStage() {
  measurement_ = Measurement{};
  measurement_.bootSessionId = gRtcState.bootSessionId;
  measurement_.sequence = nextSequence(gRtcState);
  GTH_LOGI("APP", "ACQUIRE sequence=%lu", static_cast<unsigned long>(measurement_.sequence));
  measurement_.battery = sensors_.readBattery(config_);
  measurement_.environment = sensors_.readEnvironment();
  measurement_.sonar = sensors_.readSonarBurst(config_, measurement_.environment);
  historyPending_ = true;
  prepareHealthAfterAcquisition();
}

void NodeApp::prepareHealthAfterAcquisition() {
  measurement_.healthFlags = kHealthNone;
  measurement_.qualityFlags = kQualityNone;
  if (!measurement_.environment.sensorReadOk) measurement_.healthFlags |= kDhtInvalid;
  if (!measurement_.environment.valid) measurement_.healthFlags |= kEnvironmentStale;
  if (!measurement_.environment.sensorReadOk || !measurement_.environment.valid) {
    measurement_.healthFlags |= kSensorDegraded;
  }
  if (measurement_.environment.valid) measurement_.qualityFlags |= kEnvironmentCompensated;
  if (!measurement_.sonar.valid) {
    measurement_.healthFlags |= kSonarInvalid | kSensorDegraded;
  } else {
    measurement_.qualityFlags |= kRawDistanceValid;
  }
  if (config_.installationMinimumDistanceMm != 0U) {
    measurement_.qualityFlags |= kInstallationLimitsApplied;
  }
  if (!measurement_.battery.adcValid) measurement_.healthFlags |= kBatteryAdcInvalid;
  if (measurement_.battery.low) measurement_.healthFlags |= kBatteryLow;
  if (measurement_.battery.critical) measurement_.healthFlags |= kBatteryCritical;
  if (config_.referenceDistanceMm == 0U) measurement_.healthFlags |= kCalibrationMissing;
}

void NodeApp::filterStage() {
  filterResult_ = filter_.evaluate(measurement_.sonar.medianDistanceMm,
                                   measurement_.sonar.valid, config_);
  applyFilterResult(filterResult_);
  if (filterResult_.disposition == FilterDisposition::kNeedsVerification) {
    GTH_LOGW("FILTER", "%s candidate raw=%lu previous=%lu",
             filterResult_.state == FilterState::kVerifyRise ? "apparent rise" : "apparent fall",
             static_cast<unsigned long>(filterResult_.candidateDistanceMm),
             static_cast<unsigned long>(filterResult_.acceptedDistanceMm));
  } else {
    GTH_LOGI("FILTER", "state=%s raw=%lu accepted=%lu",
             filterStateName(filterResult_.state),
             static_cast<unsigned long>(measurement_.sonar.medianDistanceMm),
             static_cast<unsigned long>(measurement_.acceptedDistanceMm));
  }
}

bool NodeApp::waitVerificationInterval(uint16_t intervalMs, bool allowButtonTransition) {
  const uint32_t started = millis();
  while (millis() - started < intervalMs) {
    delay(20);
    if (allowButtonTransition) {
      button_.update();
      if (button_.takePressedEvent()) {
        maintenanceRequested_ = true;
        return false;
      }
    }
  }
  return true;
}

void NodeApp::verifyStage(bool allowButtonTransition) {
  measurement_.qualityFlags |= kVerificationPerformed;
  const uint8_t target = filter_.verificationTarget(config_);
  const uint16_t interval = filter_.verificationIntervalMs(config_);
  GTH_LOGI("FILTER", "VERIFY observations=%u interval=%u ms", target, interval);
  while (filter_.candidateActive() && gRtcState.filter.candidateObservations < target) {
    if (!waitVerificationInterval(interval, allowButtonTransition)) break;
    const SonarBurst verification =
        sensors_.readSonarBurst(config_, measurement_.environment);
    filterResult_ = filter_.observeVerification(verification.medianDistanceMm,
                                                verification.valid, config_);
    GTH_LOGI("FILTER", "verification observation=%u/%u raw=%lu valid=%s votes=%u",
             gRtcState.filter.candidateObservations, target,
             static_cast<unsigned long>(verification.medianDistanceMm),
             verification.valid ? "yes" : "no", gRtcState.filter.candidateVotes);
  }
  if (filter_.candidateActive()) filterResult_ = filter_.finishVerification(config_);
  applyFilterResult(filterResult_);
  GTH_LOGI("FILTER", "verification result=%s candidateVotes=%u/%u accepted=%lu",
           filterStateName(filterResult_.state), gRtcState.filter.lastCandidateVotes,
           gRtcState.filter.lastCandidateObservations,
           static_cast<unsigned long>(measurement_.acceptedDistanceMm));
}

void NodeApp::applyFilterResult(const FilterResult& result) {
  measurement_.filterState = result.state;
  measurement_.acceptedDistanceMm = result.acceptedDistanceMm;
  measurement_.candidateDistanceMm = result.candidateDistanceMm;
  measurement_.healthFlags &= ~(kFilterTransient | kFilterUncertain);
  if (result.state == FilterState::kTransientRejected) {
    measurement_.healthFlags |= kFilterTransient;
  }
  if (result.state == FilterState::kUncertain || result.state == FilterState::kInvalid) {
    measurement_.healthFlags |= kFilterUncertain;
  }
  if (measurement_.acceptedDistanceMm != kDistanceUnavailable) {
    measurement_.qualityFlags |= kAcceptedDistanceValid;
  }
  updateDerivedHeight();
  sleepIntervalSec_ = result.scheduleSoon ? config_.changingWakeIntervalSec
                                          : config_.normalWakeIntervalSec;
}

void NodeApp::updateDerivedHeight() {
  if (config_.referenceDistanceMm == 0U ||
      measurement_.acceptedDistanceMm == kDistanceUnavailable) {
    measurement_.derivedWaterHeightMm = kHeightUnavailable;
    return;
  }
  measurement_.derivedWaterHeightMm = static_cast<int32_t>(config_.referenceDistanceMm) -
                                      static_cast<int32_t>(measurement_.acceptedDistanceMm);
}

protocol::TelemetryPacket NodeApp::makeTelemetry() const {
  protocol::TelemetryPacket packet{};
  strncpy(packet.nodeId, config_.nodeId, sizeof(packet.nodeId) - 1U);
  packet.bootSessionId = measurement_.bootSessionId;
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
  return packet;
}

void NodeApp::transmitStage() {
  GTH_LOGI("APP", "TRANSMIT sequence=%lu", static_cast<unsigned long>(measurement_.sequence));
  lastTx_ = radio_.sendTelemetry(makeTelemetry(), config_);
  if (!lastTx_.radioReady || !lastTx_.transmitted) measurement_.healthFlags |= kRadioError;
  if (!lastTx_.acknowledged) measurement_.healthFlags |= kTxUnacked;
}

void NodeApp::appendCurrentHistory() {
  if (!historyPending_) return;
  HistoryEntry entry{};
  entry.sequence = measurement_.sequence;
  entry.rawDistanceMm = measurement_.sonar.medianDistanceMm;
  entry.acceptedDistanceMm = measurement_.acceptedDistanceMm;
  entry.madMm = measurement_.sonar.madMm;
  entry.batteryMv = measurement_.battery.batteryMillivolts;
  entry.temperatureCentiC = toCentiTemperature(measurement_.environment.temperatureC);
  entry.humidityCentiPercent = toCentiHumidity(measurement_.environment.humidityPercent);
  entry.healthFlags = measurement_.healthFlags;
  entry.filterState = static_cast<uint8_t>(measurement_.filterState);
  entry.validSamples = measurement_.sonar.validSamples;
  entry.totalSamples = measurement_.sonar.totalSamples;
  appendHistory(gRtcState, entry);
  historyPending_ = false;
}

void NodeApp::startMaintenance() {
  if (portal_.running()) return;
  maintenanceRequested_ = false;
  if (!portal_.begin(*this)) {
    GTH_LOGE("APP", "maintenance portal unavailable; scheduling normal sleep");
    sleepIntervalSec_ = config_.changingWakeIntervalSec;
    state_ = AppState::kSleep;
  }
}

bool NodeApp::measureNow(String& error) {
  const AppState prior = state_;
  acquireStage();
  state_ = AppState::kFilter;
  filterStage();
  if (filterResult_.disposition == FilterDisposition::kNeedsVerification) {
    state_ = AppState::kVerify;
    verifyStage(false);
  }
  appendCurrentHistory();
  state_ = prior;
  error = "";
  return true;
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
    error = "radio rejected candidate settings; previous radio configuration restored";
    return false;
  }
  if (!configStore_.save(candidate)) {
    if (radioChanged) (void)radio_.applyConfig(previous);
    error = configStore_.lastError();
    return false;
  }
  config_ = candidate;
  updateDerivedHeight();
  GTH_LOGI("CONFIG", "configuration validated, applied and saved schema=%u", config_.schemaVersion);
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
  if (measurement_.sequence == 0U) {
    String ignored;
    (void)measureNow(ignored);
  }
  lastTx_ = radio_.sendTelemetry(makeTelemetry(), config_);
  if (!lastTx_.radioReady || !lastTx_.transmitted) measurement_.healthFlags |= kRadioError;
  if (!lastTx_.acknowledged) measurement_.healthFlags |= kTxUnacked;
  updateNewestHistoryHealth(gRtcState, measurement_.healthFlags);
  return lastTx_;
}

void NodeApp::requestReboot() {
  OtaManager::requestMaintenanceAfterReboot();
  rebootRequested_ = true;
}

}  // namespace gathra
