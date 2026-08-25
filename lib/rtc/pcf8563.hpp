#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ARDUINO
#include <Wire.h>
#endif

namespace gathra::rtc {

inline constexpr uint8_t kAddress = 0x51;
inline constexpr uint8_t kRegControl1 = 0x00;
inline constexpr uint8_t kRegControl2 = 0x01;
inline constexpr uint8_t kRegSeconds = 0x02;
inline constexpr uint8_t kRegMinuteAlarm = 0x09;
inline constexpr uint8_t kRegTimerControl = 0x0E;
inline constexpr uint8_t kRegTimer = 0x0F;

inline constexpr uint8_t kControl2TiTp = 1U << 4;
inline constexpr uint8_t kControl2Af = 1U << 3;
inline constexpr uint8_t kControl2Tf = 1U << 2;
inline constexpr uint8_t kControl2Aie = 1U << 1;
inline constexpr uint8_t kControl2Tie = 1U << 0;
inline constexpr uint8_t kTimerEnable = 1U << 7;
inline constexpr uint8_t kVoltageLow = 1U << 7;
inline constexpr uint8_t kAlarmDisabled = 1U << 7;

enum class TimeState : uint8_t {
  kValid = 0,
  kInvalidVl = 1,
  kUninitialized = 2,
  kI2cError = 3,
};

enum class TimerSource : uint8_t {
  k4096Hz = 0,
  k64Hz = 1,
  k1Hz = 2,
  kOnePerMinute = 3,
};

struct DateTime {
  uint16_t year = 2000;
  uint8_t month = 1;
  uint8_t day = 1;
  uint8_t weekday = 6;  // PCF8563 convention: Sunday=0.
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
};

struct Status {
  bool communicationOkay = false;
  uint8_t control1 = 0;
  uint8_t control2 = 0;
  bool timerFlag = false;
  bool alarmFlag = false;
  bool timerInterruptEnabled = false;
  bool alarmInterruptEnabled = false;
  bool levelMode = true;
};

struct TimerConfiguration {
  bool enabled = false;
  TimerSource source = TimerSource::kOnePerMinute;
  uint8_t value = 0;
  bool interruptEnabled = false;
  bool levelMode = true;
};

struct AlarmConfiguration {
  bool minuteEnabled = false;
  bool hourEnabled = false;
  bool dayEnabled = false;
  bool weekdayEnabled = false;
  uint8_t minute = 0;
  uint8_t hour = 0;
  uint8_t day = 0;
  uint8_t weekday = 0;
  bool interruptEnabled = false;
};

class RegisterIo {
 public:
  virtual ~RegisterIo() = default;
  virtual bool read(uint8_t firstRegister, uint8_t* output, size_t length) = 0;
  virtual bool write(uint8_t firstRegister, const uint8_t* input, size_t length) = 0;
};

#ifdef ARDUINO
class WireRegisterIo final : public RegisterIo {
 public:
  explicit WireRegisterIo(TwoWire& wire = Wire) : wire_(wire) {}
  bool begin(int sda, int scl, uint32_t frequencyHz = 100000U,
             uint16_t timeoutMs = 50U);
  bool read(uint8_t firstRegister, uint8_t* output, size_t length) override;
  bool write(uint8_t firstRegister, const uint8_t* input, size_t length) override;

 private:
  TwoWire& wire_;
  bool begun_ = false;
};
#endif

class Pcf8563 {
 public:
  explicit Pcf8563(RegisterIo& io) : io_(io) {}

  bool probe(Status* status = nullptr);
  bool readControl1(uint8_t& value);
  bool writeControl1(uint8_t value, bool verify = true);
  bool readControl2(uint8_t& value);
  bool writeControl2Raw(uint8_t value, bool verifyControls = true);
  bool readStatus(Status& status);

  TimeState readDateTime(DateTime& dateTime, uint32_t* unixTime = nullptr);
  bool writeDateTime(const DateTime& dateTime, bool verify = true);

  bool configureTimer(const TimerConfiguration& configuration,
                      bool preserveTimerFlag = true,
                      bool preserveAlarmFlag = true);
  bool disableTimer(bool preserveTimerFlag = true,
                    bool preserveAlarmFlag = true);
  bool readTimer(TimerConfiguration& configuration);

  bool configureAlarm(const AlarmConfiguration& configuration,
                      bool preserveTimerFlag = true,
                      bool preserveAlarmFlag = true);
  bool configureAlarmForUtc(const DateTime& target,
                            bool preserveTimerFlag = true,
                            bool preserveAlarmFlag = true);
  bool disableAlarm(bool preserveTimerFlag = true,
                    bool preserveAlarmFlag = true);
  bool readAlarm(AlarmConfiguration& configuration);

  // PCF8563 flag writes use logical-AND semantics: writing 0 clears a flag,
  // writing 1 preserves it. This is the sole low-level flag-clear primitive.
  bool clearFlags(bool clearTimerFlag, bool clearAlarmFlag);

  // Final power-release primitive. It can atomically clear selected flags and
  // disable AIE so a completed one-shot alarm cannot immediately reassert
  // INT. Callers must not use this until all next-wake verification and final
  // shutdown work has completed.
  bool releaseInterruptFlags(bool clearTimerFlag, bool clearAlarmFlag,
                             bool disableAlarmInterrupt);

  static uint8_t bcdEncode(uint8_t value);
  static bool bcdDecode(uint8_t encoded, uint8_t maximum, uint8_t& value);
  static bool dateTimeValid(const DateTime& dateTime);
  static bool dateTimeToUnix(const DateTime& dateTime, uint32_t& unixTime);
  static bool unixToDateTime(uint32_t unixTime, DateTime& dateTime);
  static bool scheduleRepresentable(uint32_t nowUnix, uint32_t targetUnix);

 private:
  bool updateInterruptControl(bool levelMode, bool timerInterrupt,
                              bool alarmInterrupt, bool preserveTimerFlag,
                              bool preserveAlarmFlag);
  RegisterIo& io_;
};

const char* timeStateName(TimeState state);

}  // namespace gathra::rtc
