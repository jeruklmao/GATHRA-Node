#include "nvs_history.hpp"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace gathra {
namespace {

uint32_t fnv(const void* data, size_t length) {
  uint32_t hash = 2166136261U;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619U;
  }
  return hash;
}

bool newerGeneration(uint32_t lhs, uint32_t rhs) {
  return static_cast<int32_t>(lhs - rhs) > 0;
}

}  // namespace

uint32_t NvsHistory::recordChecksum(const HistoryRecord& record) {
  return fnv(&record, offsetof(HistoryRecord, checksum));
}

uint32_t NvsHistory::metadataChecksum(const HistoryMetadata& metadata) {
  return fnv(&metadata, offsetof(HistoryMetadata, checksum));
}

bool NvsHistory::recordValid(const HistoryRecord& record) {
  return record.magic == kHistoryRecordMagic && record.version == kHistoryRecordVersion &&
         record.filterState <= static_cast<uint8_t>(FilterState::kInvalid) &&
         record.sequence != 0U && record.checksum == recordChecksum(record);
}

bool NvsHistory::metadataValid(const HistoryMetadata& metadata) {
  return metadata.magic == kHistoryMetadataMagic && metadata.schemaVersion == 1U &&
         metadata.structureSize == sizeof(HistoryMetadata) &&
         metadata.head < build::kHistoryCapacity &&
         metadata.count <= build::kHistoryCapacity &&
         metadata.checksum == metadataChecksum(metadata);
}

HistoryRecord NvsHistory::encode(const HistoryEntry& entry) {
  HistoryRecord record{};
  record.filterState = entry.filterState;
  record.sequence = entry.sequence;
  record.rtcUnixTime = entry.rtcUnixTime;
  record.rawDistanceMm = entry.rawDistanceMm;
  record.acceptedDistanceMm = entry.acceptedDistanceMm;
  record.madMm = entry.madMm;
  record.temperatureCentiC = entry.temperatureCentiC;
  record.humidityCentiPercent = entry.humidityCentiPercent;
  record.batteryMv = entry.batteryMv;
  record.qualityFlags = entry.qualityFlags;
  record.healthFlags = entry.healthFlags;
  record.validSamples = entry.validSamples;
  record.totalSamples = entry.totalSamples;
  record.checksum = recordChecksum(record);
  return record;
}

HistoryEntry NvsHistory::decode(const HistoryRecord& record) {
  HistoryEntry entry{};
  entry.filterState = record.filterState;
  entry.sequence = record.sequence;
  entry.rtcUnixTime = record.rtcUnixTime;
  entry.rawDistanceMm = record.rawDistanceMm;
  entry.acceptedDistanceMm = record.acceptedDistanceMm;
  entry.madMm = record.madMm;
  entry.temperatureCentiC = record.temperatureCentiC;
  entry.humidityCentiPercent = record.humidityCentiPercent;
  entry.batteryMv = record.batteryMv;
  entry.qualityFlags = record.qualityFlags;
  entry.healthFlags = record.healthFlags;
  entry.validSamples = record.validSamples;
  entry.totalSamples = record.totalSamples;
  return entry;
}

bool NvsHistory::begin(HistoryBackend& backend) {
  backend_ = &backend;
  if (!backend.begin()) {
    lastError_ = "history NVS namespace open failed";
    return false;
  }
  HistoryMetadata a{}, b{};
  const bool validA = backend.readMetadata(0U, a) && metadataValid(a);
  const bool validB = backend.readMetadata(1U, b) && metadataValid(b);
  if (validA || validB) {
    metadata_ = validA && (!validB || newerGeneration(a.generation, b.generation)) ? a : b;
    lastError_ = "history metadata loaded";
    return true;
  }
  metadata_ = HistoryMetadata{};
  metadata_.structureSize = sizeof(HistoryMetadata);
  metadata_.checksum = metadataChecksum(metadata_);
  if (!backend.writeMetadata(0U, metadata_)) {
    lastError_ = "fresh history metadata write failed";
    return false;
  }
  lastError_ = "fresh history ring initialized";
  return true;
}

bool NvsHistory::saveMetadata(const HistoryMetadata& metadata) {
  const uint8_t copy = static_cast<uint8_t>(metadata.generation & 1U);
  return backend_ != nullptr && backend_->writeMetadata(copy, metadata);
}

bool NvsHistory::append(const HistoryEntry& entry) {
  if (backend_ == nullptr || entry.sequence == 0U) {
    lastError_ = "history unavailable or sequence invalid";
    return false;
  }
  const HistoryRecord record = encode(entry);
  const uint16_t slot = metadata_.head;
  if (!backend_->writeSlot(slot, record)) {
    lastError_ = "history slot write failed";
    return false;
  }
  HistoryMetadata next = metadata_;
  ++next.generation;
  next.head = static_cast<uint16_t>((next.head + 1U) % build::kHistoryCapacity);
  if (next.count < build::kHistoryCapacity) ++next.count;
  next.checksum = metadataChecksum(next);
  if (!saveMetadata(next)) {
    lastError_ = "history slot stored but metadata commit failed";
    return false;
  }
  metadata_ = next;
  lastError_ = "history entry appended";
  return true;
}

bool NvsHistory::at(uint16_t chronologicalIndex, HistoryEntry& entry) {
  if (backend_ == nullptr || chronologicalIndex >= metadata_.count) return false;
  const uint16_t oldest = metadata_.count < build::kHistoryCapacity
                              ? 0U
                              : metadata_.head;
  const uint16_t slot = static_cast<uint16_t>(
      (oldest + chronologicalIndex) % build::kHistoryCapacity);
  HistoryRecord record{};
  if (!backend_->readSlot(slot, record) || !recordValid(record)) {
    ++corruptEntries_;
    lastError_ = "history entry missing or corrupt";
    return false;
  }
  entry = decode(record);
  return true;
}

bool NvsHistory::updateNewestHealth(uint16_t healthFlags) {
  if (backend_ == nullptr || metadata_.count == 0U) return false;
  const uint16_t slot = metadata_.head == 0U
                            ? static_cast<uint16_t>(build::kHistoryCapacity - 1U)
                            : static_cast<uint16_t>(metadata_.head - 1U);
  HistoryRecord record{};
  if (!backend_->readSlot(slot, record) || !recordValid(record)) return false;
  record.healthFlags = healthFlags;
  record.checksum = recordChecksum(record);
  return backend_->writeSlot(slot, record);
}

#ifdef ARDUINO
NvsHistoryBackend::~NvsHistoryBackend() {
  if (opened_) preferences_.end();
}

bool NvsHistoryBackend::begin() {
  if (opened_) return true;
  opened_ = preferences_.begin("gathra-hist", false,
                               build::kStoragePartition);
  return opened_;
}

bool NvsHistoryBackend::readMetadata(uint8_t copy, HistoryMetadata& metadata) {
  if (!opened_ || copy > 1U) return false;
  const char* key = copy == 0U ? "meta0" : "meta1";
  return preferences_.isKey(key) &&
         preferences_.getBytesLength(key) == sizeof(metadata) &&
         preferences_.getBytes(key, &metadata, sizeof(metadata)) == sizeof(metadata);
}

bool NvsHistoryBackend::writeMetadata(uint8_t copy, const HistoryMetadata& metadata) {
  if (!opened_ || copy > 1U) return false;
  const char* key = copy == 0U ? "meta0" : "meta1";
  return preferences_.putBytes(key, &metadata, sizeof(metadata)) == sizeof(metadata);
}

bool NvsHistoryBackend::readSlot(uint16_t slot, HistoryRecord& record) {
  if (!opened_ || slot >= build::kHistoryCapacity) return false;
  char key[8]{};
  snprintf(key, sizeof(key), "h%03u", static_cast<unsigned>(slot));
  return preferences_.isKey(key) &&
         preferences_.getBytesLength(key) == sizeof(record) &&
         preferences_.getBytes(key, &record, sizeof(record)) == sizeof(record);
}

bool NvsHistoryBackend::writeSlot(uint16_t slot, const HistoryRecord& record) {
  if (!opened_ || slot >= build::kHistoryCapacity) return false;
  char key[8]{};
  snprintf(key, sizeof(key), "h%03u", static_cast<unsigned>(slot));
  return preferences_.putBytes(key, &record, sizeof(record)) == sizeof(record);
}
#endif

}  // namespace gathra
