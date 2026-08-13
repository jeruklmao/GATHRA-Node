#pragma once

#include <Module.h>
#include <modules/SX127x/SX1278.h>

#include "model.hpp"
#include "node_config.hpp"
#include "protocol.hpp"

namespace gathra {

class LoraRadio {
 public:
  LoraRadio();
  bool begin(const NodeConfig& config);
  bool applyConfig(const NodeConfig& config);
  TxReport sendTelemetry(const protocol::TelemetryPacket& telemetry,
                         const NodeConfig& config);
  void sleep();
  bool ready() const { return ready_; }
  int16_t lastCode() const { return lastCode_; }

 private:
  bool transmit(const uint8_t* data, size_t length);
  bool waitForMatchingAck(const protocol::TelemetryPacket& telemetry,
                          uint16_t timeoutMs, TxReport& report);

  Module module_;
  SX1278 radio_;
  bool spiBegan_ = false;
  bool ready_ = false;
  int16_t lastCode_ = RADIOLIB_ERR_NONE;
};

}  // namespace gathra
