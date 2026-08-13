#pragma once

#include <stdint.h>

namespace gathra {

class RetryPolicy {
 public:
  explicit RetryPolicy(uint8_t maximumRetries)
      : maximumAttempts_(static_cast<uint8_t>(maximumRetries + 1U)) {}
  bool beginAttempt() {
    if (done_ || attempts_ >= maximumAttempts_) return false;
    ++attempts_;
    return true;
  }
  void acknowledge() { done_ = true; }
  uint8_t attempts() const { return attempts_; }
  bool exhausted() const { return !done_ && attempts_ >= maximumAttempts_; }
  bool acknowledged() const { return done_; }

 private:
  uint8_t maximumAttempts_ = 1;
  uint8_t attempts_ = 0;
  bool done_ = false;
};

}  // namespace gathra
