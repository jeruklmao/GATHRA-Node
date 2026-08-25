#pragma once

#include <stdint.h>

namespace gathra {

class Buzzer {
 public:
  void begin();
  void tick(uint32_t nowMs);
  void start(uint8_t beepCount, uint16_t onMs, uint16_t gapMs = 100U);
  void playBlocking(uint8_t beepCount, uint16_t onMs, uint16_t gapMs = 100U);
  void pollStarted() { start(1U, 100U); }
  void manualLatchConfirmed() { start(2U, 100U); }
  void pollSuccess() { start(2U, 100U); }
  void pollError() { start(3U, 100U); }
  void shutdownImminent() { start(1U, 300U); }
  void maintenanceTick(uint32_t nowMs);
  void stop();
  bool busy() const { return active_; }

 private:
  void setOutput(bool on);
  bool active_ = false;
  bool outputOn_ = false;
  uint8_t remainingBeeps_ = 0;
  uint16_t onMs_ = 0;
  uint16_t gapMs_ = 0;
  uint32_t transitionAtMs_ = 0;
  uint32_t lastMaintenanceBeepMs_ = 0;
};

}  // namespace gathra
