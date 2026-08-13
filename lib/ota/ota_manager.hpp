#pragma once

#include <stdint.h>

namespace gathra {

class OtaManager {
 public:
  void begin();
  bool partitionLayoutSane() const;
  bool pendingVerification() const { return pendingVerification_; }
  bool completeBootValidation(bool applicationStateSane);
  const char* runningPartition() const { return runningPartition_; }
  const char* imageStateName() const { return imageStateName_; }
  const char* lastStatus() const { return lastStatus_; }
  static void requestMaintenanceAfterReboot();
  static bool takeMaintenanceAfterRebootRequest();

 private:
  bool pendingVerification_ = false;
  char runningPartition_[17] = "unknown";
  const char* imageStateName_ = "UNDEFINED";
  const char* lastStatus_ = "not checked";
};

}  // namespace gathra
