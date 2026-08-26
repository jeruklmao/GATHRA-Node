#include "protocol.hpp"

#include <string.h>

namespace gathra::protocol {
namespace {

bool nodeIdByteValid(uint8_t value) {
  return (value >= 'A' && value <= 'Z') ||
         (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9') || value == '-' || value == '_';
}

class Writer {
 public:
  Writer(uint8_t* data, size_t capacity) : data_(data), capacity_(capacity) {}
  bool u8(uint8_t value) {
    if (position_ >= capacity_) return false;
    data_[position_++] = value;
    return true;
  }
  bool u16(uint16_t value) {
    return u8(static_cast<uint8_t>(value >> 8U)) &&
           u8(static_cast<uint8_t>(value));
  }
  bool u32(uint32_t value) {
    return u8(static_cast<uint8_t>(value >> 24U)) &&
           u8(static_cast<uint8_t>(value >> 16U)) &&
           u8(static_cast<uint8_t>(value >> 8U)) &&
           u8(static_cast<uint8_t>(value));
  }
  bool bytes(const uint8_t* values, size_t length) {
    if (values == nullptr || position_ + length > capacity_) return false;
    memcpy(data_ + position_, values, length);
    position_ += length;
    return true;
  }
  size_t size() const { return position_; }

 private:
  uint8_t* data_;
  size_t capacity_;
  size_t position_ = 0;
};

class Reader {
 public:
  Reader(const uint8_t* data, size_t length) : data_(data), length_(length) {}
  bool u8(uint8_t& value) {
    if (position_ >= length_) return false;
    value = data_[position_++];
    return true;
  }
  bool u16(uint16_t& value) {
    uint8_t hi = 0, lo = 0;
    if (!u8(hi) || !u8(lo)) return false;
    value = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8U) | lo);
    return true;
  }
  bool u32(uint32_t& value) {
    uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    if (!u8(b0) || !u8(b1) || !u8(b2) || !u8(b3)) return false;
    value = (static_cast<uint32_t>(b0) << 24U) |
            (static_cast<uint32_t>(b1) << 16U) |
            (static_cast<uint32_t>(b2) << 8U) | b3;
    return true;
  }
  bool bytes(uint8_t* values, size_t length) {
    if (values == nullptr || position_ + length > length_) return false;
    memcpy(values, data_ + position_, length);
    position_ += length;
    return true;
  }
  size_t remaining() const { return length_ - position_; }

 private:
  const uint8_t* data_;
  size_t length_;
  size_t position_ = 0;
};

bool writeHeader(Writer& writer, MessageType type, const char* nodeId) {
  if (!nodeIdValid(nodeId)) return false;
  const size_t length = strnlen(nodeId, build::kNodeIdCapacity);
  return writer.u8(kMagic0) && writer.u8(kMagic1) && writer.u8(kVersion) &&
         writer.u8(static_cast<uint8_t>(type)) &&
         writer.u8(static_cast<uint8_t>(length)) &&
         writer.bytes(reinterpret_cast<const uint8_t*>(nodeId), length);
}

DecodeStatus readHeader(Reader& reader, MessageType type, char* nodeId) {
  uint8_t magic0 = 0, magic1 = 0, version = 0, actualType = 0, length = 0;
  if (!reader.u8(magic0) || !reader.u8(magic1) || !reader.u8(version) ||
      !reader.u8(actualType) || !reader.u8(length)) {
    return DecodeStatus::kBufferTooSmall;
  }
  if (magic0 != kMagic0 || magic1 != kMagic1) return DecodeStatus::kBadMagic;
  if (version != kVersion) return DecodeStatus::kUnsupportedVersion;
  if (actualType != static_cast<uint8_t>(type)) return DecodeStatus::kWrongType;
  if (length == 0U || length >= build::kNodeIdCapacity ||
      reader.remaining() < length ||
      !reader.bytes(reinterpret_cast<uint8_t*>(nodeId), length)) {
    return DecodeStatus::kInvalidNodeId;
  }
  nodeId[length] = '\0';
  for (uint8_t i = 0; i < length; ++i) {
    if (!nodeIdByteValid(static_cast<uint8_t>(nodeId[i]))) {
      return DecodeStatus::kInvalidNodeId;
    }
  }
  return DecodeStatus::kOk;
}

bool validBoot(uint8_t value) {
  return value <= static_cast<uint8_t>(BootReason::kUnknown);
}
bool validRtc(uint8_t value) {
  return value <= static_cast<uint8_t>(RtcState::kI2cError);
}
bool validSchedule(uint8_t value) {
  return value <= static_cast<uint8_t>(ScheduleState::kFailed);
}
bool validCommand(uint8_t value) {
  return value <= static_cast<uint8_t>(CommandType::kSetPollIntervalMinutes);
}
bool validResult(uint8_t value, bool allowNone) {
  return value <= static_cast<uint8_t>(CommandResultCode::kInternalError) ||
         (allowNone && value == static_cast<uint8_t>(CommandResultCode::kNone));
}

uint8_t commandPayloadLength(CommandType type) {
  switch (type) {
    case CommandType::kNone:
    case CommandType::kEnterMaintenanceNow: return 0;
    case CommandType::kSetPollIntervalMinutes: return 1;
    case CommandType::kScheduleMaintenanceAt: return 4;
  }
  return UINT8_MAX;
}

bool ackCommandValid(const AckCommandPacket& packet) {
  if (!packet.timeValid && packet.gatewayUnixTime != 0U) return false;
  if (packet.commandType == CommandType::kNone) return packet.commandId == 0U;
  if (packet.commandId == 0U) return false;
  if (packet.commandType == CommandType::kSetPollIntervalMinutes) {
    return packet.pollIntervalMinutes >= 1U;
  }
  if (packet.commandType == CommandType::kScheduleMaintenanceAt) {
    return packet.scheduledMaintenanceUnix != 0U;
  }
  return packet.commandType == CommandType::kEnterMaintenanceNow;
}

}  // namespace

bool nodeIdValid(const char* nodeId) {
  if (nodeId == nullptr) return false;
  const size_t length = strnlen(nodeId, build::kNodeIdCapacity);
  if (length == 0U || length >= build::kNodeIdCapacity) return false;
  for (size_t i = 0; i < length; ++i) {
    if (!nodeIdByteValid(static_cast<uint8_t>(nodeId[i]))) return false;
  }
  return true;
}

bool encodeTelemetry(const TelemetryPacket& p, uint8_t* output, size_t capacity,
                     size_t& written) {
  written = 0;
  if (output == nullptr || p.pollIntervalMinutes == 0U) return false;
  Writer writer(output, capacity);
  if (!writeHeader(writer, MessageType::kTelemetry, p.nodeId) ||
      !writer.u32(p.persistentSessionId) || !writer.u32(p.sequence) ||
      !writer.u32(p.medianEchoUs) || !writer.u32(p.rawDistanceMm) ||
      !writer.u32(p.acceptedDistanceMm) || !writer.u16(p.madMm) ||
      !writer.u16(static_cast<uint16_t>(p.temperatureCentiC)) ||
      !writer.u16(p.humidityCentiPercent) || !writer.u16(p.batteryMv) ||
      !writer.u8(p.validSamples) || !writer.u8(p.totalSamples) ||
      !writer.u8(static_cast<uint8_t>(p.filterState)) ||
      !writer.u16(p.qualityFlags) || !writer.u16(p.healthFlags) ||
      !writer.u8(static_cast<uint8_t>(p.bootReason)) ||
      !writer.u8(static_cast<uint8_t>(p.rtcState)) || !writer.u32(p.rtcUnixTime) ||
      !writer.u8(p.pollIntervalMinutes) ||
      !writer.u8(static_cast<uint8_t>(p.scheduleState)) ||
      !writer.u32(p.scheduledMaintenanceUnix) || !writer.u32(p.lastCommandId) ||
      !writer.u8(static_cast<uint8_t>(p.lastCommandType)) ||
      !writer.u8(static_cast<uint8_t>(p.lastCommandResult)) ||
      !writer.u32(p.referenceDistanceMm)) {
    return false;
  }
  written = writer.size();
  return true;
}

DecodeStatus decodeTelemetry(const uint8_t* input, size_t length, TelemetryPacket& p) {
  if (input == nullptr) return DecodeStatus::kBufferTooSmall;
  p = TelemetryPacket{};
  Reader reader(input, length);
  DecodeStatus status = readHeader(reader, MessageType::kTelemetry, p.nodeId);
  if (status != DecodeStatus::kOk) return status;
  uint16_t temperature = 0;
  uint8_t filter = 0, boot = 0, rtc = 0, schedule = 0, command = 0, result = 0;
  if (!reader.u32(p.persistentSessionId) || !reader.u32(p.sequence) ||
      !reader.u32(p.medianEchoUs) || !reader.u32(p.rawDistanceMm) ||
      !reader.u32(p.acceptedDistanceMm) || !reader.u16(p.madMm) ||
      !reader.u16(temperature) || !reader.u16(p.humidityCentiPercent) ||
      !reader.u16(p.batteryMv) || !reader.u8(p.validSamples) ||
      !reader.u8(p.totalSamples) || !reader.u8(filter) ||
      !reader.u16(p.qualityFlags) || !reader.u16(p.healthFlags) ||
      !reader.u8(boot) || !reader.u8(rtc) || !reader.u32(p.rtcUnixTime) ||
      !reader.u8(p.pollIntervalMinutes) || !reader.u8(schedule) ||
      !reader.u32(p.scheduledMaintenanceUnix) || !reader.u32(p.lastCommandId) ||
      !reader.u8(command) || !reader.u8(result) ||
      !reader.u32(p.referenceDistanceMm)) {
    return DecodeStatus::kBufferTooSmall;
  }
  if (filter > static_cast<uint8_t>(FilterState::kInvalid) || !validBoot(boot) ||
      !validRtc(rtc) || !validSchedule(schedule) || !validCommand(command) ||
      !validResult(result, true) || p.pollIntervalMinutes == 0U) {
    return DecodeStatus::kInvalidEnum;
  }
  if ((rtc != static_cast<uint8_t>(RtcState::kValid) && p.rtcUnixTime != 0U) ||
      (schedule == static_cast<uint8_t>(ScheduleState::kNone) &&
       p.scheduledMaintenanceUnix != 0U) ||
      (command == static_cast<uint8_t>(CommandType::kNone) && p.lastCommandId != 0U)) {
    return DecodeStatus::kInvalidFlags;
  }
  p.temperatureCentiC = static_cast<int16_t>(temperature);
  p.filterState = static_cast<FilterState>(filter);
  p.bootReason = static_cast<BootReason>(boot);
  p.rtcState = static_cast<RtcState>(rtc);
  p.scheduleState = static_cast<ScheduleState>(schedule);
  p.lastCommandType = static_cast<CommandType>(command);
  p.lastCommandResult = static_cast<CommandResultCode>(result);
  return reader.remaining() == 0U ? DecodeStatus::kOk : DecodeStatus::kTrailingData;
}

bool encodeAckCommand(const AckCommandPacket& p, uint8_t* output,
                      size_t capacity, size_t& written) {
  written = 0;
  if (output == nullptr || !ackCommandValid(p)) return false;
  Writer writer(output, capacity);
  const uint8_t payloadLength = commandPayloadLength(p.commandType);
  if (!writeHeader(writer, MessageType::kAckCommand, p.nodeId) ||
      !writer.u32(p.persistentSessionId) || !writer.u32(p.sequence) ||
      !writer.u32(p.gatewayUnixTime) ||
      !writer.u8(p.timeValid ? kAckTimeValid : 0U) || !writer.u32(p.commandId) ||
      !writer.u8(static_cast<uint8_t>(p.commandType)) ||
      !writer.u8(payloadLength)) {
    return false;
  }
  if (p.commandType == CommandType::kSetPollIntervalMinutes &&
      !writer.u8(p.pollIntervalMinutes)) return false;
  if (p.commandType == CommandType::kScheduleMaintenanceAt &&
      !writer.u32(p.scheduledMaintenanceUnix)) return false;
  written = writer.size();
  return true;
}

DecodeStatus decodeAckCommand(const uint8_t* input, size_t length,
                              AckCommandPacket& p) {
  if (input == nullptr) return DecodeStatus::kBufferTooSmall;
  p = AckCommandPacket{};
  Reader reader(input, length);
  DecodeStatus status = readHeader(reader, MessageType::kAckCommand, p.nodeId);
  if (status != DecodeStatus::kOk) return status;
  uint8_t flags = 0, command = 0, payloadLength = 0;
  if (!reader.u32(p.persistentSessionId) || !reader.u32(p.sequence) ||
      !reader.u32(p.gatewayUnixTime) || !reader.u8(flags) ||
      !reader.u32(p.commandId) || !reader.u8(command) ||
      !reader.u8(payloadLength)) return DecodeStatus::kBufferTooSmall;
  if ((flags & ~kAckTimeValid) != 0U) return DecodeStatus::kInvalidFlags;
  if (!validCommand(command)) return DecodeStatus::kInvalidCommand;
  p.timeValid = (flags & kAckTimeValid) != 0U;
  p.commandType = static_cast<CommandType>(command);
  if (payloadLength != commandPayloadLength(p.commandType)) {
    return DecodeStatus::kInvalidCommand;
  }
  if (p.commandType == CommandType::kSetPollIntervalMinutes &&
      !reader.u8(p.pollIntervalMinutes)) return DecodeStatus::kBufferTooSmall;
  if (p.commandType == CommandType::kScheduleMaintenanceAt &&
      !reader.u32(p.scheduledMaintenanceUnix)) return DecodeStatus::kBufferTooSmall;
  if (!ackCommandValid(p)) return DecodeStatus::kInvalidCommand;
  return reader.remaining() == 0U ? DecodeStatus::kOk : DecodeStatus::kTrailingData;
}

bool encodeCommandResult(const CommandResultPacket& p, uint8_t* output,
                         size_t capacity, size_t& written) {
  written = 0;
  if (output == nullptr || p.commandId == 0U ||
      p.commandType == CommandType::kNone ||
      !validResult(static_cast<uint8_t>(p.resultCode), false)) return false;
  Writer writer(output, capacity);
  if (!writeHeader(writer, MessageType::kCommandResult, p.nodeId) ||
      !writer.u32(p.persistentSessionId) || !writer.u32(p.commandId) ||
      !writer.u8(static_cast<uint8_t>(p.commandType)) ||
      !writer.u8(static_cast<uint8_t>(p.resultCode)) ||
      !writer.u8(p.effectivePollIntervalMinutes) ||
      !writer.u32(p.scheduledMaintenanceUnix)) return false;
  written = writer.size();
  return true;
}

DecodeStatus decodeCommandResult(const uint8_t* input, size_t length,
                                 CommandResultPacket& p) {
  if (input == nullptr) return DecodeStatus::kBufferTooSmall;
  p = CommandResultPacket{};
  Reader reader(input, length);
  DecodeStatus status = readHeader(reader, MessageType::kCommandResult, p.nodeId);
  if (status != DecodeStatus::kOk) return status;
  uint8_t command = 0, result = 0;
  if (!reader.u32(p.persistentSessionId) || !reader.u32(p.commandId) ||
      !reader.u8(command) || !reader.u8(result) ||
      !reader.u8(p.effectivePollIntervalMinutes) ||
      !reader.u32(p.scheduledMaintenanceUnix)) return DecodeStatus::kBufferTooSmall;
  if (!validCommand(command) || command == 0U || !validResult(result, false) ||
      p.commandId == 0U) return DecodeStatus::kInvalidCommand;
  p.commandType = static_cast<CommandType>(command);
  p.resultCode = static_cast<CommandResultCode>(result);
  return reader.remaining() == 0U ? DecodeStatus::kOk : DecodeStatus::kTrailingData;
}

bool ackMatches(const AckCommandPacket& ack, const TelemetryPacket& telemetry) {
  return strncmp(ack.nodeId, telemetry.nodeId, build::kNodeIdCapacity) == 0 &&
         ack.persistentSessionId == telemetry.persistentSessionId &&
         ack.sequence == telemetry.sequence;
}

const char* decodeStatusName(DecodeStatus status) {
  switch (status) {
    case DecodeStatus::kOk: return "OK";
    case DecodeStatus::kBufferTooSmall: return "BUFFER_TOO_SMALL";
    case DecodeStatus::kBadMagic: return "BAD_MAGIC";
    case DecodeStatus::kUnsupportedVersion: return "UNSUPPORTED_VERSION";
    case DecodeStatus::kWrongType: return "WRONG_TYPE";
    case DecodeStatus::kInvalidNodeId: return "INVALID_NODE_ID";
    case DecodeStatus::kInvalidEnum: return "INVALID_ENUM";
    case DecodeStatus::kInvalidFlags: return "INVALID_FLAGS";
    case DecodeStatus::kInvalidCommand: return "INVALID_COMMAND";
    case DecodeStatus::kTrailingData: return "TRAILING_DATA";
  }
  return "UNKNOWN";
}

const char* bootReasonName(BootReason reason) {
  switch (reason) {
    case BootReason::kRtcTimer: return "RTC_TIMER";
    case BootReason::kRtcScheduledMaintenance: return "RTC_SCHEDULED_MAINTENANCE";
    case BootReason::kManualButton: return "MANUAL_BUTTON";
    case BootReason::kMaintenanceReboot: return "MAINTENANCE_REBOOT";
    case BootReason::kOtaReboot: return "OTA_REBOOT";
    case BootReason::kUnknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* rtcStateName(RtcState state) {
  switch (state) {
    case RtcState::kValid: return "VALID";
    case RtcState::kInvalidVl: return "INVALID_VL";
    case RtcState::kUninitialized: return "UNINITIALIZED";
    case RtcState::kI2cError: return "I2C_ERROR";
  }
  return "I2C_ERROR";
}

const char* scheduleStateName(ScheduleState state) {
  switch (state) {
    case ScheduleState::kNone: return "NONE";
    case ScheduleState::kPending: return "PENDING";
    case ScheduleState::kCompleted: return "COMPLETED";
    case ScheduleState::kFailed: return "FAILED";
  }
  return "FAILED";
}

const char* commandTypeName(CommandType type) {
  switch (type) {
    case CommandType::kNone: return "NONE";
    case CommandType::kEnterMaintenanceNow: return "ENTER_MAINTENANCE_NOW";
    case CommandType::kScheduleMaintenanceAt: return "SCHEDULE_MAINTENANCE_AT";
    case CommandType::kSetPollIntervalMinutes: return "SET_POLL_INTERVAL_MINUTES";
  }
  return "UNKNOWN";
}

const char* commandResultName(CommandResultCode result) {
  switch (result) {
    case CommandResultCode::kApplied: return "APPLIED";
    case CommandResultCode::kAlreadyApplied: return "ALREADY_APPLIED";
    case CommandResultCode::kInvalidArgument: return "INVALID_ARGUMENT";
    case CommandResultCode::kRtcUnavailable: return "RTC_UNAVAILABLE";
    case CommandResultCode::kRtcTimeUntrusted: return "RTC_TIME_UNTRUSTED";
    case CommandResultCode::kScheduleUnrepresentable: return "SCHEDULE_UNREPRESENTABLE";
    case CommandResultCode::kStorageError: return "STORAGE_ERROR";
    case CommandResultCode::kInternalError: return "INTERNAL_ERROR";
    case CommandResultCode::kNone: return "NONE";
  }
  return "UNKNOWN";
}

}  // namespace gathra::protocol
