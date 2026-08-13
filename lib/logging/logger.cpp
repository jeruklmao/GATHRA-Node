#include "logger.hpp"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace gathra {
namespace {

const char* levelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo: return "INFO";
    case LogLevel::kWarn: return "WARN";
    case LogLevel::kError: return "ERROR";
  }
  return "UNKNOWN";
}

}  // namespace

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

void Logger::begin() {
  count_ = 0;
  head_ = 0;
}

void Logger::log(LogLevel level, const char* tag, const char* format, ...) {
  char message[104]{};
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);

  char line[kLineLength]{};
  snprintf(line, sizeof(line), "[%10lu][%s][%s] %s",
           static_cast<unsigned long>(millis()), levelName(level), tag, message);
  Serial.println(line);
  strncpy(lines_[head_], line, kLineLength - 1U);
  lines_[head_][kLineLength - 1U] = '\0';
  head_ = (head_ + 1U) % build::kLogCapacity;
  if (count_ < build::kLogCapacity) ++count_;
}

bool Logger::at(uint8_t chronologicalIndex, char* output, size_t capacity) const {
  if (output == nullptr || capacity == 0U || chronologicalIndex >= count_) return false;
  const uint8_t oldest = count_ < build::kLogCapacity ? 0U : head_;
  strncpy(output, lines_[(oldest + chronologicalIndex) % build::kLogCapacity], capacity - 1U);
  output[capacity - 1U] = '\0';
  return true;
}

}  // namespace gathra
