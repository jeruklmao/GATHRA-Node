#include "power_manager.hpp"

#include <Arduino.h>
#include <WiFi.h>

#include "board_pins.hpp"
#include "esp_sleep.h"
#include "esp_system.h"
#include "logger.hpp"

namespace gathra {

const char* PowerManager::wakeReasonName() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "UNDEFINED/COLD_BOOT";
    case ESP_SLEEP_WAKEUP_ALL: return "ALL";
    case ESP_SLEEP_WAKEUP_EXT0: return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1: return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER: return "TIMER";
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return "TOUCHPAD";
    case ESP_SLEEP_WAKEUP_ULP: return "ULP";
    case ESP_SLEEP_WAKEUP_GPIO: return "GPIO_BUTTON";
    case ESP_SLEEP_WAKEUP_UART: return "UART";
    case ESP_SLEEP_WAKEUP_WIFI: return "WIFI";
    case ESP_SLEEP_WAKEUP_COCPU: return "COCPU";
    case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG: return "COCPU_TRAP";
    case ESP_SLEEP_WAKEUP_BT: return "BT";
  }
  return "UNKNOWN";
}

const char* PowerManager::resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_UNKNOWN: return "UNKNOWN";
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
  }
  return "UNKNOWN";
}

bool PowerManager::wokeFromButton() {
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
}

[[noreturn]] void PowerManager::deepSleep(uint32_t seconds) {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  GTH_LOGI("POWER", "waiting for active-low button release");
  pinMode(board::kButton, INPUT_PULLUP);
  while (digitalRead(board::kButton) == LOW) delay(20);
  const esp_err_t timerResult =
      esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
  const esp_err_t gpioResult = esp_deep_sleep_enable_gpio_wakeup(
      1ULL << board::kButton, ESP_GPIO_WAKEUP_GPIO_LOW);
  GTH_LOGI("POWER", "deep sleep in 100 ms for %lu s timer=%s gpio=%s",
           static_cast<unsigned long>(seconds), esp_err_to_name(timerResult),
           esp_err_to_name(gpioResult));
  if (timerResult != ESP_OK || gpioResult != ESP_OK) {
    GTH_LOGE("POWER", "wake-source setup failed; restarting instead of entering unsafe sleep");
    Serial.flush();
    delay(100);
    esp_restart();
  }
  Serial.flush();
  delay(100);
  esp_deep_sleep_start();
  __builtin_unreachable();
}

}  // namespace gathra
