#include "buzzer.hpp"

#include <Arduino.h>

#include "board_pins.hpp"

namespace gathra {

void Buzzer::begin() {
  pinMode(board::kBuzzer, OUTPUT);
  stop();
}

void Buzzer::setOutput(bool on) {
  outputOn_ = on;
  digitalWrite(board::kBuzzer, on == board::kBuzzerActiveHigh ? HIGH : LOW);
}

void Buzzer::start(uint8_t beepCount, uint16_t onMs, uint16_t gapMs) {
  if (beepCount == 0U || onMs == 0U) {
    stop();
    return;
  }
  active_ = true;
  remainingBeeps_ = beepCount;
  onMs_ = onMs;
  gapMs_ = gapMs;
  setOutput(true);
  transitionAtMs_ = millis() + onMs_;
}

void Buzzer::tick(uint32_t nowMs) {
  if (!active_ || static_cast<int32_t>(nowMs - transitionAtMs_) < 0) return;
  if (outputOn_) {
    setOutput(false);
    if (--remainingBeeps_ == 0U) {
      active_ = false;
      return;
    }
    transitionAtMs_ = nowMs + gapMs_;
  } else {
    setOutput(true);
    transitionAtMs_ = nowMs + onMs_;
  }
}

void Buzzer::playBlocking(uint8_t beepCount, uint16_t onMs, uint16_t gapMs) {
  start(beepCount, onMs, gapMs);
  while (busy()) {
    tick(millis());
    delay(1);
  }
  stop();
}

void Buzzer::maintenanceTick(uint32_t nowMs) {
  tick(nowMs);
  if (!busy() && (lastMaintenanceBeepMs_ == 0U ||
                  nowMs - lastMaintenanceBeepMs_ >= 3000U)) {
    lastMaintenanceBeepMs_ = nowMs;
    start(1U, 100U);
  }
}

void Buzzer::stop() {
  active_ = false;
  remainingBeeps_ = 0U;
  setOutput(false);
}

}  // namespace gathra
