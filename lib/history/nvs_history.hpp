#pragma once

#include <stddef.h>
#include <stdint.h>

#include "build_config.hpp"
#include "model.hpp"

#ifdef ARDUINO
#include <Preferences.h>
#endif

namespace gathra {

inline constexpr uint16_t kHistoryRecordMagic = 0x4832U;  // H2
inline constexpr uint8_t kHistoryRecordVersion = 1U;
inline constexpr uint32_t kHistoryMetadataMagic = 0x4748324DU;  // GH2M

struct HistoryEntry {
  uint32_t sequence = 0;
  uint32_t rtcUnixTime = 0;
  uint32_t rawDistanceMm = kDistanceUnavailable;
  uint32_t acceptedDistanceMm = kDistanceUnavailable;
  uint16_t madMm = 0;
  int16_t temperatureCentiC = kTemperatureUnavailable;
  uint16_t humidityCentiPercent = kHumidityUnavailable;
  uint16_t batteryMv = 0;
  uint16_t qualityFlags = 0;
  uint16_t healthFlags = 0;
  uint8_t filterState = static_cast<uint8_t>(FilterState::kInvalid);
  uint8_t validSamples = 0;
  uint8_t totalSamples = 0;
};

struct __attribute__((packed)) HistoryRecord {
  uint16_t magic = kHistoryRecordMagic;
  uint8_t version = kHistoryRecordVersion;
  uint8_t filterState = static_cast<uint8_t>(FilterState::kInvalid);
  uint32_t sequence = 0;
  uint32_t rtcUnixTime = 0;
  uint32_t rawDistanceMm = kDistanceUnavailable;
  uint32_t acceptedDistanceMm = kDistanceUnavailable;
  uint16_t madMm = 0;
  int16_t temperatureCentiC = kTemperatureUnavailable;
  uint16_t humidityCentiPercent = kHumidityUnavailable;
  uint16_t batteryMv = 0;
  uint16_t qualityFlags = 0;
  uint16_t healthFlags = 0;
  uint8_t validSamples = 0;
  uint8_t totalSamples = 0;
  uint32_t checksum = 0;
};

struct HistoryMetadata {
  uint32_t magic = kHistoryMetadataMagic;
  uint16_t schemaVersion = 1;
  uint16_t structureSize = 0;
  uint32_t generation = 0;
  uint16_t head = 0;
  uint16_t count = 0;
  uint32_t checksum = 0;
};

static_assert(sizeof(HistoryRecord) == 38U, "history record wire/storage size changed");

class HistoryBackend {
 public:
  virtual ~HistoryBackend() = default;
  virtual bool begin() = 0;
  virtual bool readMetadata(uint8_t copy, HistoryMetadata& metadata) = 0;
  virtual bool writeMetadata(uint8_t copy, const HistoryMetadata& metadata) = 0;
  virtual bool readSlot(uint16_t slot, HistoryRecord& record) = 0;
  virtual bool writeSlot(uint16_t slot, const HistoryRecord& record) = 0;
};

#ifdef ARDUINO
class NvsHistoryBackend final : public HistoryBackend {
 public:
  ~NvsHistoryBackend() override;
  bool begin() override;
  bool readMetadata(uint8_t copy, HistoryMetadata& metadata) override;
  bool writeMetadata(uint8_t copy, const HistoryMetadata& metadata) override;
  bool readSlot(uint16_t slot, HistoryRecord& record) override;
  bool writeSlot(uint16_t slot, const HistoryRecord& record) override;

 private:
  Preferences preferences_;
  bool opened_ = false;
};
#endif

class NvsHistory {
 public:
  bool begin(HistoryBackend& backend);
  bool append(const HistoryEntry& entry);
  bool at(uint16_t chronologicalIndex, HistoryEntry& entry);
  bool updateNewestHealth(uint16_t healthFlags);
  uint16_t count() const { return metadata_.count; }
  static constexpr uint16_t capacity() { return build::kHistoryCapacity; }
  uint32_t corruptEntries() const { return corruptEntries_; }
  const char* lastError() const { return lastError_; }

  static uint32_t recordChecksum(const HistoryRecord& record);
  static uint32_t metadataChecksum(const HistoryMetadata& metadata);
  static bool recordValid(const HistoryRecord& record);
  static bool metadataValid(const HistoryMetadata& metadata);
  static HistoryRecord encode(const HistoryEntry& entry);
  static HistoryEntry decode(const HistoryRecord& record);

 private:
  bool saveMetadata(const HistoryMetadata& metadata);
  HistoryBackend* backend_ = nullptr;
  HistoryMetadata metadata_{};
  uint32_t corruptEntries_ = 0;
  const char* lastError_ = "not initialized";
};

}  // namespace gathra
