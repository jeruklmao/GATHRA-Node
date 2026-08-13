#pragma once

#include <stdint.h>

namespace gathra {

class Button {
 public:
  void begin();
  void update();
  bool takePressedEvent();
  bool isPressed() const { return stablePressed_; }
 bool longPressed(uint32_t thresholdMs) const;

 private:
  static void buttonIsrThunk(void* argument);
  void handleButtonInterrupt();

  bool rawPressed_ = false;
  bool stablePressed_ = false;
  bool pressedEvent_ = false;
  bool currentPressReported_ = false;
  uint32_t rawChangedMs_ = 0;
  uint32_t pressedSinceMs_ = 0;
  volatile bool irqPressActive_ = false;
  volatile bool irqCompletedPress_ = false;
  volatile uint32_t irqPressStartedTick_ = 0;
};

}  // namespace gathra
