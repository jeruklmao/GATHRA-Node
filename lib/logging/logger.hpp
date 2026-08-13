#pragma once

#include <stddef.h>
#include <stdint.h>

#include "build_config.hpp"

namespace gathra {

enum class LogLevel : uint8_t { kDebug, kInfo, kWarn, kError };

class Logger {
 public:
  static Logger& instance();
  void begin();
  void log(LogLevel level, const char* tag, const char* format, ...)
      __attribute__((format(printf, 4, 5)));
  uint8_t count() const { return count_; }
  bool at(uint8_t chronologicalIndex, char* output, size_t capacity) const;

 private:
  static constexpr size_t kLineLength = 144;
  char lines_[build::kLogCapacity][kLineLength]{};
  uint8_t count_ = 0;
  uint8_t head_ = 0;
};

#define GTH_LOGD(tag, fmt, ...) \
  ::gathra::Logger::instance().log(::gathra::LogLevel::kDebug, tag, fmt, ##__VA_ARGS__)
#define GTH_LOGI(tag, fmt, ...) \
  ::gathra::Logger::instance().log(::gathra::LogLevel::kInfo, tag, fmt, ##__VA_ARGS__)
#define GTH_LOGW(tag, fmt, ...) \
  ::gathra::Logger::instance().log(::gathra::LogLevel::kWarn, tag, fmt, ##__VA_ARGS__)
#define GTH_LOGE(tag, fmt, ...) \
  ::gathra::Logger::instance().log(::gathra::LogLevel::kError, tag, fmt, ##__VA_ARGS__)

}  // namespace gathra
