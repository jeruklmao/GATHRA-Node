#include "srf05_driver.hpp"

#include <Arduino.h>

#include "build_config.hpp"
#include "esp_timer.h"
#include "logger.hpp"

namespace gathra {

bool Srf05Driver::begin() {
  ownerTask_ = xTaskGetCurrentTaskHandle();
  gpio_config_t trigger{};
  trigger.pin_bit_mask = 1ULL << static_cast<uint32_t>(triggerPin_);
  trigger.mode = GPIO_MODE_OUTPUT;
  trigger.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&trigger) != ESP_OK || gpio_set_level(triggerPin_, 0) != ESP_OK) return false;

  gpio_config_t echo{};
  echo.pin_bit_mask = 1ULL << static_cast<uint32_t>(echoPin_);
  echo.mode = GPIO_MODE_INPUT;
  echo.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&echo) != ESP_OK) return false;
  // Use Arduino's shared GPIO ISR service so RadioLib can attach DIO0 without
  // attempting to install a second service. Capture itself remains raw GPIO
  // any-edge ISR state plus an ISR-to-owner-task notification.
  attachInterruptArg(static_cast<uint8_t>(echoPin_), &Srf05Driver::echoIsrThunk,
                     this, CHANGE);
  initialized_ = true;
  GTH_LOGI("SONAR", "native ISR capture initialized");
  return true;
}

EchoSample Srf05Driver::measure(uint32_t timeoutUs) {
  EchoSample result{};
  if (!initialized_ || ownerTask_ == nullptr) {
    result.status = EchoStatus::kNotInitialized;
    return result;
  }
  (void)ulTaskNotifyTake(pdTRUE, 0);
  resetCapture();
  if (gpio_get_level(echoPin_) != 0) {
    result.status = EchoStatus::kEchoHighBeforeTrigger;
    return result;
  }
  captureActive_ = true;
  gpio_set_level(triggerPin_, 0);
  delayMicroseconds(2);
  gpio_set_level(triggerPin_, 1);
  delayMicroseconds(10);
  gpio_set_level(triggerPin_, 0);
  triggerEndUs_ = static_cast<uint32_t>(esp_timer_get_time());

  TickType_t ticks = pdMS_TO_TICKS((timeoutUs + 999U) / 1000U);
  if (ticks == 0U) ticks = 1U;
  const uint32_t notified = ulTaskNotifyTake(pdTRUE, ticks);
  captureActive_ = false;
  result.risingEdges = risingEdges_;
  result.fallingEdges = fallingEdges_;
  if (notified == 0U) {
    result.status = sawRise_ ? EchoStatus::kNoFallingEdge : EchoStatus::kNoRisingEdge;
    return result;
  }
  if (!sawRise_) {
    result.status = EchoStatus::kNoRisingEdge;
    return result;
  }
  if (!sawFall_) {
    result.status = EchoStatus::kNoFallingEdge;
    return result;
  }
  result.triggerToEchoUs = riseUs_ - triggerEndUs_;
  result.pulseUs = fallUs_ - riseUs_;
  if (result.pulseUs < build::kMinimumEchoUs) {
    result.status = EchoStatus::kPulseTooShort;
  } else if (result.pulseUs > build::kMaximumEchoUs) {
    result.status = EchoStatus::kPulseTooLong;
  } else {
    result.status = EchoStatus::kOk;
  }
  return result;
}

void Srf05Driver::echoIsrThunk(void* argument) {
  static_cast<Srf05Driver*>(argument)->handleEchoInterrupt();
}

void Srf05Driver::handleEchoInterrupt() {
  if (!captureActive_) return;
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
  if (gpio_get_level(echoPin_) != 0) {
    if (risingEdges_ != UINT8_MAX) ++risingEdges_;
    if (!sawRise_) {
      riseUs_ = now;
      sawRise_ = true;
    }
    return;
  }
  if (fallingEdges_ != UINT8_MAX) ++fallingEdges_;
  if (!sawRise_ || sawFall_) return;
  fallUs_ = now;
  sawFall_ = true;
  captureActive_ = false;
  BaseType_t higherPriorityWoken = pdFALSE;
  vTaskNotifyGiveFromISR(ownerTask_, &higherPriorityWoken);
  if (higherPriorityWoken == pdTRUE) portYIELD_FROM_ISR();
}

void Srf05Driver::resetCapture() {
  captureActive_ = false;
  sawRise_ = false;
  sawFall_ = false;
  triggerEndUs_ = 0;
  riseUs_ = 0;
  fallUs_ = 0;
  risingEdges_ = 0;
  fallingEdges_ = 0;
}

const char* Srf05Driver::statusName(EchoStatus status) {
  switch (status) {
    case EchoStatus::kOk: return "OK";
    case EchoStatus::kNotInitialized: return "NOT_INITIALIZED";
    case EchoStatus::kEchoHighBeforeTrigger: return "ECHO_HIGH_BEFORE_TRIGGER";
    case EchoStatus::kNoRisingEdge: return "NO_RISING_EDGE";
    case EchoStatus::kNoFallingEdge: return "NO_FALLING_EDGE";
    case EchoStatus::kPulseTooShort: return "PULSE_TOO_SHORT";
    case EchoStatus::kPulseTooLong: return "PULSE_TOO_LONG";
    case EchoStatus::kOutsideInstallationRange: return "OUTSIDE_INSTALLATION_RANGE";
  }
  return "UNKNOWN";
}

}  // namespace gathra
