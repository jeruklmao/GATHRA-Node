#include "model.hpp"

namespace gathra {

const char* appStateName(AppState state) {
  switch (state) {
    case AppState::kBoot: return "BOOT";
    case AppState::kAcquire: return "ACQUIRE";
    case AppState::kFilter: return "FILTER";
    case AppState::kVerify: return "VERIFY";
    case AppState::kTransmit: return "TRANSMIT";
    case AppState::kMaintenance: return "MAINTENANCE";
    case AppState::kPowerOff: return "POWER_OFF";
    case AppState::kFault: return "FAULT";
  }
  return "UNKNOWN";
}

const char* filterStateName(FilterState state) {
  switch (state) {
    case FilterState::kStable: return "STABLE";
    case FilterState::kAccepted: return "ACCEPTED";
    case FilterState::kVerifyRise: return "VERIFY_RISE";
    case FilterState::kVerifyFall: return "VERIFY_FALL";
    case FilterState::kTransientRejected: return "TRANSIENT_REJECTED";
    case FilterState::kChangeConfirmed: return "CHANGE_CONFIRMED";
    case FilterState::kUncertain: return "UNCERTAIN";
    case FilterState::kInvalid: return "INVALID";
  }
  return "UNKNOWN";
}

}  // namespace gathra
