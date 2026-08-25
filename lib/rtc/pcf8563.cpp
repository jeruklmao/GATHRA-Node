#include "pcf8563.hpp"

#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace gathra::rtc {
namespace {

constexpr uint32_t kUnix2000 = 946684800U;
constexpr uint32_t kUnix2100 = 4102444800U;
constexpr uint32_t kMaximumScheduleHorizonSec = 27U * 24U * 60U * 60U;

bool leapYear(uint16_t year) {
  return (year % 4U) == 0U && ((year % 100U) != 0U || (year % 400U) == 0U);
}

uint8_t daysInMonth(uint16_t year, uint8_t month) {
  constexpr uint8_t days[] = {31, 28, 31, 30, 31, 30,
                              31, 31, 30, 31, 30, 31};
  if (month == 0U || month > 12U) return 0U;
  return month == 2U && leapYear(year) ? 29U : days[month - 1U];
}

// Days since 1970-01-01. These are the public-domain civil-calendar
// transformations described by Howard Hinnant, restricted here to UTC.
int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2U;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153U * (month + (month > 2U ? -3 : 9)) + 2U) / 5U + day - 1U;
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int& year, unsigned& month, unsigned& day) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
  year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
  const unsigned mp = (5U * doy + 2U) / 153U;
  day = doy - (153U * mp + 2U) / 5U + 1U;
  month = mp + (mp < 10U ? 3U : static_cast<unsigned>(-9));
  year += month <= 2U;
}

bool alarmDecode(uint8_t encoded, uint8_t maximum, bool& enabled, uint8_t& value) {
  enabled = (encoded & kAlarmDisabled) == 0U;
  if (!enabled) {
    value = 0U;
    return (encoded & 0x7FU) == 0U;
  }
  return Pcf8563::bcdDecode(encoded & 0x7FU, maximum, value);
}

}  // namespace

#ifdef ARDUINO
bool WireRegisterIo::begin(int sda, int scl, uint32_t frequencyHz,
                           uint16_t timeoutMs) {
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  begun_ = wire_.begin(sda, scl, frequencyHz);
  wire_.setTimeOut(timeoutMs);
  return begun_;
}

bool WireRegisterIo::read(uint8_t firstRegister, uint8_t* output, size_t length) {
  if (!begun_ || output == nullptr || length == 0U || length > 32U) return false;
  wire_.beginTransmission(kAddress);
  if (wire_.write(firstRegister) != 1U || wire_.endTransmission(false) != 0U) return false;
  const size_t received = wire_.requestFrom(kAddress, length, true);
  if (received != length) {
    while (wire_.available()) (void)wire_.read();
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!wire_.available()) return false;
    output[i] = static_cast<uint8_t>(wire_.read());
  }
  return true;
}

bool WireRegisterIo::write(uint8_t firstRegister, const uint8_t* input,
                            size_t length) {
  if (!begun_ || input == nullptr || length == 0U || length > 31U) return false;
  wire_.beginTransmission(kAddress);
  if (wire_.write(firstRegister) != 1U || wire_.write(input, length) != length) {
    (void)wire_.endTransmission(true);
    return false;
  }
  return wire_.endTransmission(true) == 0U;
}
#endif

uint8_t Pcf8563::bcdEncode(uint8_t value) {
  return static_cast<uint8_t>(((value / 10U) << 4U) | (value % 10U));
}

bool Pcf8563::bcdDecode(uint8_t encoded, uint8_t maximum, uint8_t& value) {
  const uint8_t tens = static_cast<uint8_t>((encoded >> 4U) & 0x0FU);
  const uint8_t units = static_cast<uint8_t>(encoded & 0x0FU);
  if (tens > 9U || units > 9U) return false;
  value = static_cast<uint8_t>(tens * 10U + units);
  return value <= maximum;
}

bool Pcf8563::dateTimeValid(const DateTime& d) {
  return d.year >= 2000U && d.year <= 2099U && d.month >= 1U && d.month <= 12U &&
         d.day >= 1U && d.day <= daysInMonth(d.year, d.month) && d.weekday <= 6U &&
         d.hour <= 23U && d.minute <= 59U && d.second <= 59U;
}

bool Pcf8563::dateTimeToUnix(const DateTime& d, uint32_t& unixTime) {
  if (!dateTimeValid(d)) return false;
  const int64_t seconds = daysFromCivil(d.year, d.month, d.day) * 86400LL +
                          static_cast<int64_t>(d.hour) * 3600LL +
                          static_cast<int64_t>(d.minute) * 60LL + d.second;
  if (seconds < kUnix2000 || seconds >= kUnix2100) return false;
  unixTime = static_cast<uint32_t>(seconds);
  return true;
}

bool Pcf8563::unixToDateTime(uint32_t unixTime, DateTime& d) {
  if (unixTime < kUnix2000 || unixTime >= kUnix2100) return false;
  const uint32_t days = unixTime / 86400U;
  const uint32_t inDay = unixTime % 86400U;
  int year = 0;
  unsigned month = 0, day = 0;
  civilFromDays(days, year, month, day);
  d.year = static_cast<uint16_t>(year);
  d.month = static_cast<uint8_t>(month);
  d.day = static_cast<uint8_t>(day);
  d.hour = static_cast<uint8_t>(inDay / 3600U);
  d.minute = static_cast<uint8_t>((inDay % 3600U) / 60U);
  d.second = static_cast<uint8_t>(inDay % 60U);
  d.weekday = static_cast<uint8_t>((days + 4U) % 7U);  // 1970-01-01 Thursday.
  return dateTimeValid(d);
}

bool Pcf8563::scheduleRepresentable(uint32_t nowUnix, uint32_t targetUnix) {
  DateTime now{}, target{};
  if (!unixToDateTime(nowUnix, now) || !unixToDateTime(targetUnix, target)) return false;
  if (target.second != 0U || targetUnix < nowUnix + 60U) return false;
  return targetUnix - nowUnix <= kMaximumScheduleHorizonSec;
}

bool Pcf8563::probe(Status* status) {
  Status observed{};
  if (!readStatus(observed)) return false;
  if (status != nullptr) *status = observed;
  return true;
}

bool Pcf8563::readControl1(uint8_t& value) { return io_.read(kRegControl1, &value, 1U); }

bool Pcf8563::writeControl1(uint8_t value, bool verify) {
  value &= 0x28U;  // STOP and TESTC only; all N bits must be zero.
  if (!io_.write(kRegControl1, &value, 1U)) return false;
  if (!verify) return true;
  uint8_t readback = 0;
  return readControl1(readback) && readback == value;
}

bool Pcf8563::readControl2(uint8_t& value) { return io_.read(kRegControl2, &value, 1U); }

bool Pcf8563::writeControl2Raw(uint8_t value, bool verifyControls) {
  value &= 0x1FU;
  if (!io_.write(kRegControl2, &value, 1U)) return false;
  if (!verifyControls) return true;
  uint8_t readback = 0;
  constexpr uint8_t controls = kControl2TiTp | kControl2Aie | kControl2Tie;
  return readControl2(readback) && (readback & controls) == (value & controls);
}

bool Pcf8563::readStatus(Status& status) {
  uint8_t values[2]{};
  if (!io_.read(kRegControl1, values, sizeof(values))) {
    status = Status{};
    return false;
  }
  status.communicationOkay = true;
  status.control1 = values[0];
  status.control2 = values[1];
  status.timerFlag = (values[1] & kControl2Tf) != 0U;
  status.alarmFlag = (values[1] & kControl2Af) != 0U;
  status.timerInterruptEnabled = (values[1] & kControl2Tie) != 0U;
  status.alarmInterruptEnabled = (values[1] & kControl2Aie) != 0U;
  status.levelMode = (values[1] & kControl2TiTp) == 0U;
  return true;
}

TimeState Pcf8563::readDateTime(DateTime& d, uint32_t* unixTime) {
  uint8_t bytes[7]{};
  if (!io_.read(kRegSeconds, bytes, sizeof(bytes))) return TimeState::kI2cError;
  const bool vl = (bytes[0] & kVoltageLow) != 0U;
  uint8_t year = 0;
  if (!bcdDecode(bytes[0] & 0x7FU, 59U, d.second) ||
      !bcdDecode(bytes[1] & 0x7FU, 59U, d.minute) ||
      !bcdDecode(bytes[2] & 0x3FU, 23U, d.hour) ||
      !bcdDecode(bytes[3] & 0x3FU, 31U, d.day) ||
      (d.weekday = static_cast<uint8_t>(bytes[4] & 0x07U)) > 6U ||
      !bcdDecode(bytes[5] & 0x1FU, 12U, d.month) ||
      !bcdDecode(bytes[6], 99U, year)) {
    return TimeState::kUninitialized;
  }
  d.year = static_cast<uint16_t>(2000U + year);
  uint32_t converted = 0;
  if (!dateTimeValid(d) || !dateTimeToUnix(d, converted)) return TimeState::kUninitialized;
  if (unixTime != nullptr) *unixTime = vl ? 0U : converted;
  return vl ? TimeState::kInvalidVl : TimeState::kValid;
}

bool Pcf8563::writeDateTime(const DateTime& d, bool verify) {
  if (!dateTimeValid(d)) return false;
  const uint8_t bytes[] = {
      bcdEncode(d.second), bcdEncode(d.minute), bcdEncode(d.hour), bcdEncode(d.day),
      static_cast<uint8_t>(d.weekday & 0x07U), bcdEncode(d.month),
      bcdEncode(static_cast<uint8_t>(d.year - 2000U))};
  if (!io_.write(kRegSeconds, bytes, sizeof(bytes))) return false;
  if (!verify) return true;
  DateTime readback{};
  uint32_t expected = 0, actual = 0;
  return dateTimeToUnix(d, expected) && readDateTime(readback, &actual) == TimeState::kValid &&
         actual >= expected && actual <= expected + 1U;
}

bool Pcf8563::updateInterruptControl(bool levelMode, bool timerInterrupt,
                                     bool alarmInterrupt, bool preserveTimerFlag,
                                     bool preserveAlarmFlag) {
  uint8_t value = 0;
  if (!levelMode) value |= kControl2TiTp;
  if (timerInterrupt) value |= kControl2Tie;
  if (alarmInterrupt) value |= kControl2Aie;
  if (preserveTimerFlag) value |= kControl2Tf;
  if (preserveAlarmFlag) value |= kControl2Af;
  return writeControl2Raw(value, true);
}

bool Pcf8563::configureTimer(const TimerConfiguration& c,
                             bool preserveTimerFlag, bool preserveAlarmFlag) {
  if (!c.enabled || c.value == 0U) return false;
  uint8_t status = 0;
  if (!readControl2(status)) return false;
  const uint8_t disabled = static_cast<uint8_t>(c.source);
  if (!io_.write(kRegTimerControl, &disabled, 1U) ||
      !io_.write(kRegTimer, &c.value, 1U) ||
      !updateInterruptControl(c.levelMode, c.interruptEnabled,
                              (status & kControl2Aie) != 0U,
                              preserveTimerFlag, preserveAlarmFlag)) return false;
  const uint8_t enabled = static_cast<uint8_t>(kTimerEnable | static_cast<uint8_t>(c.source));
  if (!io_.write(kRegTimerControl, &enabled, 1U)) return false;
  uint8_t controlReadback = 0, timerReadback = 0, statusReadback = 0;
  if (!io_.read(kRegTimerControl, &controlReadback, 1U) ||
      !io_.read(kRegTimer, &timerReadback, 1U) ||
      !readControl2(statusReadback)) return false;
  const bool timerValuePlausible = timerReadback > 0U && timerReadback <= c.value;
  const bool alreadyElapsed = (statusReadback & kControl2Tf) != 0U;
  return controlReadback == enabled && (timerValuePlausible || alreadyElapsed) &&
         ((statusReadback & kControl2Tie) != 0U) == c.interruptEnabled &&
         ((statusReadback & kControl2TiTp) == 0U) == c.levelMode;
}

bool Pcf8563::disableTimer(bool preserveTimerFlag, bool preserveAlarmFlag) {
  uint8_t status = 0;
  if (!readControl2(status)) return false;
  const uint8_t disabled = static_cast<uint8_t>(TimerSource::kOnePerMinute);
  if (!io_.write(kRegTimerControl, &disabled, 1U) ||
      !updateInterruptControl(true, false, (status & kControl2Aie) != 0U,
                              preserveTimerFlag, preserveAlarmFlag)) return false;
  uint8_t readback = 0;
  return io_.read(kRegTimerControl, &readback, 1U) && readback == disabled;
}

bool Pcf8563::readTimer(TimerConfiguration& c) {
  uint8_t bytes[2]{}, status = 0;
  if (!io_.read(kRegTimerControl, bytes, sizeof(bytes)) || !readControl2(status)) return false;
  c.enabled = (bytes[0] & kTimerEnable) != 0U;
  c.source = static_cast<TimerSource>(bytes[0] & 0x03U);
  c.value = bytes[1];
  c.interruptEnabled = (status & kControl2Tie) != 0U;
  c.levelMode = (status & kControl2TiTp) == 0U;
  return true;
}

bool Pcf8563::configureAlarm(const AlarmConfiguration& c,
                             bool preserveTimerFlag, bool preserveAlarmFlag) {
  if ((!c.minuteEnabled && !c.hourEnabled && !c.dayEnabled && !c.weekdayEnabled) ||
      (c.minuteEnabled && c.minute > 59U) || (c.hourEnabled && c.hour > 23U) ||
      (c.dayEnabled && (c.day == 0U || c.day > 31U)) ||
      (c.weekdayEnabled && c.weekday > 6U)) return false;
  const uint8_t bytes[] = {
      c.minuteEnabled ? bcdEncode(c.minute) : kAlarmDisabled,
      c.hourEnabled ? bcdEncode(c.hour) : kAlarmDisabled,
      c.dayEnabled ? bcdEncode(c.day) : kAlarmDisabled,
      c.weekdayEnabled ? c.weekday : kAlarmDisabled};
  uint8_t status = 0;
  if (!readControl2(status) || !io_.write(kRegMinuteAlarm, bytes, sizeof(bytes)) ||
      !updateInterruptControl((status & kControl2TiTp) == 0U,
                              (status & kControl2Tie) != 0U, c.interruptEnabled,
                              preserveTimerFlag, preserveAlarmFlag)) return false;
  uint8_t readback[4]{}, statusReadback = 0;
  return io_.read(kRegMinuteAlarm, readback, sizeof(readback)) &&
         memcmp(bytes, readback, sizeof(bytes)) == 0 && readControl2(statusReadback) &&
         ((statusReadback & kControl2Aie) != 0U) == c.interruptEnabled;
}

bool Pcf8563::configureAlarmForUtc(const DateTime& target,
                                   bool preserveTimerFlag, bool preserveAlarmFlag) {
  if (!dateTimeValid(target) || target.second != 0U) return false;
  AlarmConfiguration alarm{};
  alarm.minuteEnabled = alarm.hourEnabled = alarm.dayEnabled = true;
  alarm.minute = target.minute;
  alarm.hour = target.hour;
  alarm.day = target.day;
  alarm.interruptEnabled = true;
  return configureAlarm(alarm, preserveTimerFlag, preserveAlarmFlag);
}

bool Pcf8563::disableAlarm(bool preserveTimerFlag, bool preserveAlarmFlag) {
  const uint8_t bytes[] = {kAlarmDisabled, kAlarmDisabled, kAlarmDisabled, kAlarmDisabled};
  uint8_t status = 0;
  if (!readControl2(status) || !io_.write(kRegMinuteAlarm, bytes, sizeof(bytes)) ||
      !updateInterruptControl((status & kControl2TiTp) == 0U,
                              (status & kControl2Tie) != 0U, false,
                              preserveTimerFlag, preserveAlarmFlag)) return false;
  uint8_t readback[4]{};
  return io_.read(kRegMinuteAlarm, readback, sizeof(readback)) &&
         memcmp(bytes, readback, sizeof(bytes)) == 0;
}

bool Pcf8563::readAlarm(AlarmConfiguration& c) {
  uint8_t bytes[4]{}, status = 0;
  if (!io_.read(kRegMinuteAlarm, bytes, sizeof(bytes)) || !readControl2(status)) return false;
  if (!alarmDecode(bytes[0], 59U, c.minuteEnabled, c.minute) ||
      !alarmDecode(bytes[1], 23U, c.hourEnabled, c.hour) ||
      !alarmDecode(bytes[2], 31U, c.dayEnabled, c.day)) return false;
  c.weekdayEnabled = (bytes[3] & kAlarmDisabled) == 0U;
  c.weekday = static_cast<uint8_t>(bytes[3] & 0x07U);
  if ((c.weekdayEnabled && c.weekday > 6U) ||
      (!c.weekdayEnabled && (bytes[3] & 0x7FU) != 0U)) return false;
  c.interruptEnabled = (status & kControl2Aie) != 0U;
  return true;
}

bool Pcf8563::clearFlags(bool clearTimerFlag, bool clearAlarmFlag) {
  return releaseInterruptFlags(clearTimerFlag, clearAlarmFlag, false);
}

bool Pcf8563::releaseInterruptFlags(bool clearTimerFlag, bool clearAlarmFlag,
                                    bool disableAlarmInterrupt) {
  uint8_t current = 0;
  if (!readControl2(current)) return false;
  uint8_t value = static_cast<uint8_t>(current &
      (kControl2TiTp | kControl2Aie | kControl2Tie));
  if (disableAlarmInterrupt) value &= static_cast<uint8_t>(~kControl2Aie);
  if (!clearTimerFlag) value |= kControl2Tf;
  if (!clearAlarmFlag) value |= kControl2Af;
  if (!writeControl2Raw(value, true)) return false;
  uint8_t readback = 0;
  if (!readControl2(readback)) return false;
  if (clearTimerFlag && (readback & kControl2Tf) != 0U) return false;
  if (clearAlarmFlag && (readback & kControl2Af) != 0U) return false;
  if (disableAlarmInterrupt && (readback & kControl2Aie) != 0U) return false;
  return true;
}

const char* timeStateName(TimeState state) {
  switch (state) {
    case TimeState::kValid: return "VALID";
    case TimeState::kInvalidVl: return "INVALID_VL";
    case TimeState::kUninitialized: return "UNINITIALIZED";
    case TimeState::kI2cError: return "I2C_ERROR";
  }
  return "I2C_ERROR";
}

}  // namespace gathra::rtc
