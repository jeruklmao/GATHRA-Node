#pragma once

#include <stdint.h>

namespace gathra {

class PowerManager {
 public:
  static const char* wakeReasonName();
  static const char* resetReasonName();
  static bool wokeFromButton();
  [[noreturn]] static void deepSleep(uint32_t seconds);
};

}  // namespace gathra
