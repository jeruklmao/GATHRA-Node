#pragma once

#include <stddef.h>
#include <stdint.h>

#include "build_config.hpp"
#include "model.hpp"

namespace gathra::protocol {

inline constexpr uint8_t kMagic0 = 0x47;  // G
inline constexpr uint8_t kMagic1 = 0x54;  // T
inline constexpr uint8_t kVersion = 1;

enum class MessageType : uint8_t {
  kTelemetry = 1,
  kAck = 2,
};

enum class DecodeStatus : uint8_t {
  kOk,
  kBufferTooSmall,
  kBadMagic,
  kUnsupportedVersion,
  kWrongType,
  kInvalidNodeId,
  kTrailingData,
};

struct TelemetryPacket {
  char nodeId[build::kNodeIdCapacity]{};
  uint32_t bootSessionId = 0;
  uint32_t sequence = 0;
  uint32_t medianEchoUs = 0;
  uint32_t rawDistanceMm = kDistanceUnavailable;
  uint32_t acceptedDistanceMm = kDistanceUnavailable;
  uint16_t madMm = 0;
  int16_t temperatureCentiC = kTemperatureUnavailable;
  uint16_t humidityCentiPercent = kHumidityUnavailable;
  uint16_t batteryMv = 0;
  uint8_t validSamples = 0;
  uint8_t totalSamples = 0;
  FilterState filterState = FilterState::kInvalid;
  uint16_t qualityFlags = 0;
  uint16_t healthFlags = 0;
};

struct AckPacket {
  char nodeId[build::kNodeIdCapacity]{};
  uint32_t bootSessionId = 0;
  uint32_t sequence = 0;
};

bool encodeTelemetry(const TelemetryPacket& packet, uint8_t* output,
                     size_t capacity, size_t& written);
DecodeStatus decodeTelemetry(const uint8_t* input, size_t length,
                             TelemetryPacket& packet);
bool encodeAck(const AckPacket& packet, uint8_t* output, size_t capacity,
               size_t& written);
DecodeStatus decodeAck(const uint8_t* input, size_t length, AckPacket& packet);
bool ackMatches(const AckPacket& ack, const TelemetryPacket& telemetry);
const char* decodeStatusName(DecodeStatus status);

}  // namespace gathra::protocol
