#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model.hpp"

namespace gathra {

class Srf05Driver {
 public:
  Srf05Driver(gpio_num_t triggerPin, gpio_num_t echoPin)
      : triggerPin_(triggerPin), echoPin_(echoPin) {}
  bool begin();
  EchoSample measure(uint32_t timeoutUs);
  static const char* statusName(EchoStatus status);

 private:
  static void echoIsrThunk(void* argument);
  void handleEchoInterrupt();
  void resetCapture();

  const gpio_num_t triggerPin_;
  const gpio_num_t echoPin_;
  TaskHandle_t ownerTask_ = nullptr;
  bool initialized_ = false;
  volatile bool captureActive_ = false;
  volatile bool sawRise_ = false;
  volatile bool sawFall_ = false;
  volatile uint32_t triggerEndUs_ = 0;
  volatile uint32_t riseUs_ = 0;
  volatile uint32_t fallUs_ = 0;
  volatile uint8_t risingEdges_ = 0;
  volatile uint8_t fallingEdges_ = 0;
};

}  // namespace gathra
