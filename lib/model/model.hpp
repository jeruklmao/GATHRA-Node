#pragma once

#include <math.h>
#include <stdint.h>

#include "build_config.hpp"

namespace gathra {

inline constexpr uint32_t kDistanceUnavailable = UINT32_MAX;
inline constexpr int32_t kHeightUnavailable = INT32_MIN;
inline constexpr int16_t kTemperatureUnavailable = INT16_MIN;
inline constexpr uint16_t kHumidityUnavailable = UINT16_MAX;

enum class AppState : uint8_t {
  kBoot,
  kAcquire,
  kFilter,
  kVerify,
  kTransmit,
  kMaintenance,
  kSleep,
};

enum class FilterState : uint8_t {
  kStable = 0,
  kAccepted = 1,
  kVerifyRise = 2,
  kVerifyFall = 3,
  kTransientRejected = 4,
  kChangeConfirmed = 5,
  kUncertain = 6,
  kInvalid = 7,
};

enum HealthFlag : uint16_t {
  kHealthNone = 0,
  kSonarInvalid = 1U << 0,
  kDhtInvalid = 1U << 1,
  kEnvironmentStale = 1U << 2,
  kBatteryLow = 1U << 3,
  kBatteryCritical = 1U << 4,
  kRadioError = 1U << 5,
  kTxUnacked = 1U << 6,
  kFilterTransient = 1U << 7,
  kFilterUncertain = 1U << 8,
  kCalibrationMissing = 1U << 9,
  kSensorDegraded = 1U << 10,
  kBatteryAdcInvalid = 1U << 11,
};

enum QualityFlag : uint16_t {
  kQualityNone = 0,
  kEnvironmentCompensated = 1U << 0,
  kRawDistanceValid = 1U << 1,
  kAcceptedDistanceValid = 1U << 2,
  kInstallationLimitsApplied = 1U << 3,
  kVerificationPerformed = 1U << 4,
};

struct EnvironmentReading {
  bool sensorReadOk = false;
  bool valid = false;
  bool stale = true;
  float temperatureC = NAN;
  float humidityPercent = NAN;
  float speedOfSoundMps = 343.0F;
  uint32_t ageMs = UINT32_MAX;
};

enum class EchoStatus : uint8_t {
  kOk,
  kNotInitialized,
  kEchoHighBeforeTrigger,
  kNoRisingEdge,
  kNoFallingEdge,
  kPulseTooShort,
  kPulseTooLong,
  kOutsideInstallationRange,
};

struct EchoSample {
  EchoStatus status = EchoStatus::kNotInitialized;
  uint32_t pulseUs = 0;
  uint32_t triggerToEchoUs = 0;
  uint16_t distanceMm = 0;
  uint8_t risingEdges = 0;
  uint8_t fallingEdges = 0;
};

struct SonarBurst {
  bool valid = false;
  uint8_t validSamples = 0;
  uint8_t totalSamples = 0;
  uint32_t medianEchoUs = 0;
  uint32_t medianDistanceMm = kDistanceUnavailable;
  uint16_t madMm = 0;
  EchoSample samples[build::kMaximumSonarSamples]{};
};

struct BatteryReading {
  bool adcValid = false;
  uint16_t adcRaw = 0;
  uint16_t adcMillivolts = 0;
  uint16_t batteryMillivolts = 0;
  bool low = false;
  bool critical = false;
};

struct Measurement {
  uint32_t bootSessionId = 0;
  uint32_t sequence = 0;
  SonarBurst sonar{};
  EnvironmentReading environment{};
  BatteryReading battery{};
  uint32_t acceptedDistanceMm = kDistanceUnavailable;
  int32_t derivedWaterHeightMm = kHeightUnavailable;
  uint32_t candidateDistanceMm = kDistanceUnavailable;
  FilterState filterState = FilterState::kInvalid;
  uint16_t healthFlags = kHealthNone;
  uint16_t qualityFlags = kQualityNone;
};

struct TxReport {
  bool radioReady = false;
  bool transmitted = false;
  bool acknowledged = false;
  uint8_t attempts = 0;
  int16_t lastRadioCode = 0;
  float lastRssi = NAN;
  float lastSnr = NAN;
};

const char* appStateName(AppState state);
const char* filterStateName(FilterState state);

}  // namespace gathra
