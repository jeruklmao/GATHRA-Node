#include "protocol.hpp"

#include <string.h>

namespace gathra::protocol {
namespace {

class Writer {
 public:
  Writer(uint8_t* data, size_t capacity) : data_(data), capacity_(capacity) {}
  bool u8(uint8_t value) {
    if (position_ >= capacity_) return false;
    data_[position_++] = value;
    return true;
  }
  bool u16(uint16_t value) {
    return u8(static_cast<uint8_t>(value >> 8U)) && u8(static_cast<uint8_t>(value));
  }
  bool u32(uint32_t value) {
    return u8(static_cast<uint8_t>(value >> 24U)) &&
           u8(static_cast<uint8_t>(value >> 16U)) &&
           u8(static_cast<uint8_t>(value >> 8U)) && u8(static_cast<uint8_t>(value));
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
            (static_cast<uint32_t>(b2) << 8U) | static_cast<uint32_t>(b3);
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

bool writeHeaderAndNode(Writer& writer, MessageType type, const char* nodeId) {
  const size_t nodeLength = strnlen(nodeId, build::kNodeIdCapacity);
  if (nodeLength == 0U || nodeLength >= build::kNodeIdCapacity) return false;
  return writer.u8(kMagic0) && writer.u8(kMagic1) && writer.u8(kVersion) &&
         writer.u8(static_cast<uint8_t>(type)) && writer.u8(static_cast<uint8_t>(nodeLength)) &&
         writer.bytes(reinterpret_cast<const uint8_t*>(nodeId), nodeLength);
}

DecodeStatus readHeaderAndNode(Reader& reader, MessageType expectedType, char* nodeId) {
  uint8_t magic0 = 0, magic1 = 0, version = 0, type = 0, nodeLength = 0;
  if (!reader.u8(magic0) || !reader.u8(magic1) || !reader.u8(version) ||
      !reader.u8(type) || !reader.u8(nodeLength)) {
    return DecodeStatus::kBufferTooSmall;
  }
  if (magic0 != kMagic0 || magic1 != kMagic1) return DecodeStatus::kBadMagic;
  if (version != kVersion) return DecodeStatus::kUnsupportedVersion;
  if (type != static_cast<uint8_t>(expectedType)) return DecodeStatus::kWrongType;
  if (nodeLength == 0U || nodeLength >= build::kNodeIdCapacity ||
      reader.remaining() < nodeLength) {
    return DecodeStatus::kInvalidNodeId;
  }
  if (!reader.bytes(reinterpret_cast<uint8_t*>(nodeId), nodeLength)) {
    return DecodeStatus::kBufferTooSmall;
  }
  nodeId[nodeLength] = '\0';
  return DecodeStatus::kOk;
}

}  // namespace

bool encodeTelemetry(const TelemetryPacket& p, uint8_t* output, size_t capacity,
                     size_t& written) {
  written = 0;
  if (output == nullptr) return false;
  Writer writer(output, capacity);
  if (!writeHeaderAndNode(writer, MessageType::kTelemetry, p.nodeId) ||
      !writer.u32(p.bootSessionId) || !writer.u32(p.sequence) ||
      !writer.u32(p.medianEchoUs) || !writer.u32(p.rawDistanceMm) ||
      !writer.u32(p.acceptedDistanceMm) || !writer.u16(p.madMm) ||
      !writer.u16(static_cast<uint16_t>(p.temperatureCentiC)) ||
      !writer.u16(p.humidityCentiPercent) || !writer.u16(p.batteryMv) ||
      !writer.u8(p.validSamples) || !writer.u8(p.totalSamples) ||
      !writer.u8(static_cast<uint8_t>(p.filterState)) ||
      !writer.u16(p.qualityFlags) || !writer.u16(p.healthFlags)) {
    return false;
  }
  written = writer.size();
  return true;
}

DecodeStatus decodeTelemetry(const uint8_t* input, size_t length, TelemetryPacket& p) {
  if (input == nullptr) return DecodeStatus::kBufferTooSmall;
  p = TelemetryPacket{};
  Reader reader(input, length);
  DecodeStatus status = readHeaderAndNode(reader, MessageType::kTelemetry, p.nodeId);
  if (status != DecodeStatus::kOk) return status;
  uint16_t temperature = 0;
  uint8_t filterByte = 0;
  if (!reader.u32(p.bootSessionId) || !reader.u32(p.sequence) ||
      !reader.u32(p.medianEchoUs) || !reader.u32(p.rawDistanceMm) ||
      !reader.u32(p.acceptedDistanceMm) || !reader.u16(p.madMm) ||
      !reader.u16(temperature) || !reader.u16(p.humidityCentiPercent) ||
      !reader.u16(p.batteryMv) || !reader.u8(p.validSamples) ||
      !reader.u8(p.totalSamples) || !reader.u8(filterByte) ||
      !reader.u16(p.qualityFlags) || !reader.u16(p.healthFlags)) {
    return DecodeStatus::kBufferTooSmall;
  }
  p.temperatureCentiC = static_cast<int16_t>(temperature);
  p.filterState = static_cast<FilterState>(filterByte);
  return reader.remaining() == 0U ? DecodeStatus::kOk : DecodeStatus::kTrailingData;
}

bool encodeAck(const AckPacket& p, uint8_t* output, size_t capacity, size_t& written) {
  written = 0;
  if (output == nullptr) return false;
  Writer writer(output, capacity);
  if (!writeHeaderAndNode(writer, MessageType::kAck, p.nodeId) ||
      !writer.u32(p.bootSessionId) || !writer.u32(p.sequence)) {
    return false;
  }
  written = writer.size();
  return true;
}

DecodeStatus decodeAck(const uint8_t* input, size_t length, AckPacket& p) {
  if (input == nullptr) return DecodeStatus::kBufferTooSmall;
  p = AckPacket{};
  Reader reader(input, length);
  DecodeStatus status = readHeaderAndNode(reader, MessageType::kAck, p.nodeId);
  if (status != DecodeStatus::kOk) return status;
  if (!reader.u32(p.bootSessionId) || !reader.u32(p.sequence)) {
    return DecodeStatus::kBufferTooSmall;
  }
  return reader.remaining() == 0U ? DecodeStatus::kOk : DecodeStatus::kTrailingData;
}

bool ackMatches(const AckPacket& ack, const TelemetryPacket& telemetry) {
  return strncmp(ack.nodeId, telemetry.nodeId, build::kNodeIdCapacity) == 0 &&
         ack.bootSessionId == telemetry.bootSessionId && ack.sequence == telemetry.sequence;
}

const char* decodeStatusName(DecodeStatus status) {
  switch (status) {
    case DecodeStatus::kOk: return "OK";
    case DecodeStatus::kBufferTooSmall: return "BUFFER_TOO_SMALL";
    case DecodeStatus::kBadMagic: return "BAD_MAGIC";
    case DecodeStatus::kUnsupportedVersion: return "UNSUPPORTED_VERSION";
    case DecodeStatus::kWrongType: return "WRONG_TYPE";
    case DecodeStatus::kInvalidNodeId: return "INVALID_NODE_ID";
    case DecodeStatus::kTrailingData: return "TRAILING_DATA";
  }
  return "UNKNOWN";
}

}  // namespace gathra::protocol
