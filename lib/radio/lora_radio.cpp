#include "lora_radio.hpp"

#include <Arduino.h>
#include <SPI.h>
#include <string.h>

#include "board_pins.hpp"
#include "build_config.hpp"
#include "esp_system.h"
#include "logger.hpp"
#include "retry_policy.hpp"

namespace gathra {
namespace {

volatile bool gRadioIrq = false;
void IRAM_ATTR radioInterrupt() { gRadioIrq = true; }

}  // namespace

LoraRadio::LoraRadio()
    : module_(board::kRadioCs, board::kRadioDio0, board::kRadioReset, RADIOLIB_NC),
      radio_(&module_) {}

bool LoraRadio::begin(const NodeConfig& config) {
  if (!spiBegan_) {
    SPI.begin(board::kRadioSck, board::kRadioMiso, board::kRadioMosi,
              board::kRadioCs);
    spiBegan_ = true;
  }
  return applyConfig(config);
}

bool LoraRadio::applyConfig(const NodeConfig& config) {
  if (ready_) (void)radio_.standby();
  lastCode_ = radio_.begin(config.loraFrequencyMhz, config.loraBandwidthKhz,
                           config.loraSpreadingFactor,
                           config.loraCodingRateDenominator, config.loraSyncWord,
                           config.loraTxPowerDbm);
  ready_ = lastCode_ == RADIOLIB_ERR_NONE;
  if (ready_) {
    lastCode_ = radio_.setCRC(true);
    ready_ = lastCode_ == RADIOLIB_ERR_NONE;
  }
  if (ready_) {
    (void)radio_.standby();
    GTH_LOGI("RADIO", "SX1278 ready %.3f MHz BW %.1f SF%u CR4/%u %d dBm sync=0x%02X",
             config.loraFrequencyMhz, config.loraBandwidthKhz,
             config.loraSpreadingFactor, config.loraCodingRateDenominator,
             config.loraTxPowerDbm, config.loraSyncWord);
  } else {
    GTH_LOGE("RADIO", "SX1278 initialization/configuration failed code=%d", lastCode_);
  }
  return ready_;
}

bool LoraRadio::transmit(const uint8_t* data, size_t length) {
  gRadioIrq = false;
  radio_.clearPacketReceivedAction();
  radio_.setPacketSentAction(radioInterrupt);
  lastCode_ = radio_.startTransmit(data, length);
  if (lastCode_ != RADIOLIB_ERR_NONE) return false;
  const uint32_t started = millis();
  uint32_t timeoutMs = radio_.getTimeOnAir(length) / 1000U + 1500U;
  if (timeoutMs < 2500U) timeoutMs = 2500U;
  while (!gRadioIrq && millis() - started < timeoutMs) delay(1);
  if (!gRadioIrq) {
    (void)radio_.standby();
    lastCode_ = RADIOLIB_ERR_TX_TIMEOUT;
    return false;
  }
  gRadioIrq = false;
  lastCode_ = radio_.finishTransmit();
  return lastCode_ == RADIOLIB_ERR_NONE;
}

bool LoraRadio::waitForMatchingAck(const protocol::TelemetryPacket& telemetry,
                                   uint16_t timeoutMs, TxReport& report) {
  gRadioIrq = false;
  radio_.clearPacketSentAction();
  radio_.setPacketReceivedAction(radioInterrupt);
  lastCode_ = radio_.startReceive();
  if (lastCode_ != RADIOLIB_ERR_NONE) return false;
  const uint32_t started = millis();
  while (!gRadioIrq && millis() - started < timeoutMs) delay(1);
  if (!gRadioIrq) {
    (void)radio_.standby();
    GTH_LOGW("RADIO", "ACK timeout after %u ms", timeoutMs);
    return false;
  }
  gRadioIrq = false;
  const size_t packetLength = radio_.getPacketLength();
  if (packetLength == 0U || packetLength > build::kRadioPacketCapacity) {
    (void)radio_.standby();
    GTH_LOGW("RADIO", "received invalid ACK length=%u", static_cast<unsigned>(packetLength));
    return false;
  }
  uint8_t buffer[build::kRadioPacketCapacity]{};
  lastCode_ = radio_.readData(buffer, packetLength);
  if (lastCode_ != RADIOLIB_ERR_NONE) {
    GTH_LOGW("RADIO", "ACK read failed code=%d", lastCode_);
    return false;
  }
  report.lastRssi = radio_.getRSSI();
  report.lastSnr = radio_.getSNR();
  protocol::AckPacket ack{};
  const protocol::DecodeStatus decode = protocol::decodeAck(buffer, packetLength, ack);
  const bool matches = decode == protocol::DecodeStatus::kOk &&
                       protocol::ackMatches(ack, telemetry);
  GTH_LOGI("RADIO", "ACK received decode=%s match=%s RSSI=%.1f SNR=%.1f",
           protocol::decodeStatusName(decode), matches ? "yes" : "no",
           report.lastRssi, report.lastSnr);
  return matches;
}

TxReport LoraRadio::sendTelemetry(const protocol::TelemetryPacket& telemetry,
                                  const NodeConfig& config) {
  TxReport report{};
  report.radioReady = ready_;
  if (!ready_ && !begin(config)) {
    report.lastRadioCode = lastCode_;
    return report;
  }
  uint8_t payload[build::kRadioPacketCapacity]{};
  size_t length = 0;
  if (!protocol::encodeTelemetry(telemetry, payload, sizeof(payload), length)) {
    lastCode_ = RADIOLIB_ERR_PACKET_TOO_LONG;
    report.lastRadioCode = lastCode_;
    return report;
  }
  RetryPolicy retries(config.ackRetryCount);
  while (retries.beginAttempt()) {
    report.attempts = retries.attempts();
    GTH_LOGI("RADIO", "telemetry TX sequence=%lu attempt=%u bytes=%u",
             static_cast<unsigned long>(telemetry.sequence), report.attempts,
             static_cast<unsigned>(length));
    if (!transmit(payload, length)) {
      GTH_LOGW("RADIO", "TX failed attempt=%u code=%d", report.attempts, lastCode_);
    } else {
      report.transmitted = true;
      if (waitForMatchingAck(telemetry, config.ackTimeoutMs, report)) {
        retries.acknowledge();
        report.acknowledged = true;
        break;
      }
    }
    if (!retries.exhausted()) {
      const uint32_t backoffMs = 100U + (esp_random() % 251U);
      GTH_LOGW("RADIO", "retry backoff=%lu ms", static_cast<unsigned long>(backoffMs));
      delay(backoffMs);
    }
  }
  (void)radio_.standby();
  report.lastRadioCode = lastCode_;
  if (!report.acknowledged) {
    GTH_LOGW("RADIO", "telemetry unacknowledged after %u attempt(s)", report.attempts);
  }
  return report;
}

void LoraRadio::sleep() {
  if (!ready_) return;
  radio_.clearPacketReceivedAction();
  radio_.clearPacketSentAction();
  (void)radio_.standby();
  lastCode_ = radio_.sleep();
}

}  // namespace gathra
