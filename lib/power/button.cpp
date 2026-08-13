#include "button.hpp"

#include <Arduino.h>

#include "board_pins.hpp"
#include "build_config.hpp"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace gathra {

void Button::begin() {
  pinMode(board::kButton, INPUT_PULLUP);
  rawPressed_ = digitalRead(board::kButton) == LOW;
  stablePressed_ = rawPressed_;
  rawChangedMs_ = millis();
  pressedSinceMs_ = stablePressed_ ? millis() : 0U;
  pressedEvent_ = false;
  currentPressReported_ = stablePressed_;
  attachInterruptArg(board::kButton, &Button::buttonIsrThunk, this, CHANGE);
}

void Button::update() {
  noInterrupts();
  const bool completedWhileBusy = irqCompletedPress_;
  irqCompletedPress_ = false;
  interrupts();
  if (completedWhileBusy && !currentPressReported_) pressedEvent_ = true;

  const bool raw = digitalRead(board::kButton) == LOW;
  const uint32_t now = millis();
  if (raw != rawPressed_) {
    rawPressed_ = raw;
    rawChangedMs_ = now;
  }
  if (rawPressed_ != stablePressed_ && now - rawChangedMs_ >= build::kButtonDebounceMs) {
    stablePressed_ = rawPressed_;
    if (stablePressed_) {
      pressedSinceMs_ = now;
      pressedEvent_ = true;
      currentPressReported_ = true;
    } else {
      pressedSinceMs_ = 0U;
      currentPressReported_ = false;
    }
  }
}

void Button::buttonIsrThunk(void* argument) {
  static_cast<Button*>(argument)->handleButtonInterrupt();
}

void IRAM_ATTR Button::handleButtonInterrupt() {
  const bool pressed = gpio_get_level(static_cast<gpio_num_t>(board::kButton)) == 0;
  const uint32_t now = static_cast<uint32_t>(xTaskGetTickCountFromISR());
  if (pressed) {
    irqPressStartedTick_ = now;
    irqPressActive_ = true;
    return;
  }
  if (irqPressActive_ && now - irqPressStartedTick_ >= pdMS_TO_TICKS(build::kButtonDebounceMs)) {
    irqCompletedPress_ = true;
  }
  irqPressActive_ = false;
}

bool Button::takePressedEvent() {
  const bool event = pressedEvent_;
  pressedEvent_ = false;
  return event;
}

bool Button::longPressed(uint32_t thresholdMs) const {
  return stablePressed_ && pressedSinceMs_ != 0U && millis() - pressedSinceMs_ >= thresholdMs;
}

}  // namespace gathra
