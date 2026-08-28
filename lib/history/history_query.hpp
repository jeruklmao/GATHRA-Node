#pragma once

#include <stdint.h>

namespace gathra::history_query {

inline constexpr uint16_t kDefaultPageSize = 12U;
inline constexpr uint16_t kMaximumPageSize = 12U;
inline constexpr uint16_t kDefaultChartPoints = 100U;
inline constexpr uint16_t kMaximumChartPoints = 100U;

struct Page {
  uint16_t offset = 0;
  uint16_t limit = 0;
  uint16_t returned = 0;
  uint16_t total = 0;
  uint16_t nextOffset = 0;
  bool hasPrevious = false;
  bool hasNext = false;
};

inline Page page(uint16_t total, uint16_t requestedOffset,
                 uint16_t requestedLimit) {
  Page result{};
  result.total = total;
  result.offset = requestedOffset > total ? total : requestedOffset;
  result.limit = requestedLimit == 0U ? kDefaultPageSize : requestedLimit;
  if (result.limit > kMaximumPageSize) result.limit = kMaximumPageSize;
  const uint16_t remaining = static_cast<uint16_t>(total - result.offset);
  result.returned = remaining < result.limit ? remaining : result.limit;
  result.nextOffset = static_cast<uint16_t>(result.offset + result.returned);
  result.hasPrevious = result.offset != 0U;
  result.hasNext = result.nextOffset < total;
  return result;
}

inline uint16_t chartPointCount(uint16_t total, uint16_t requested) {
  uint16_t points = requested == 0U ? kDefaultChartPoints : requested;
  if (points > kMaximumChartPoints) points = kMaximumChartPoints;
  return total < points ? total : points;
}

// Returns a chronological NvsHistory index. When downsampling, both endpoints
// are retained and intermediate points are distributed uniformly.
inline uint16_t chartIndex(uint16_t total, uint16_t points,
                           uint16_t pointIndex) {
  if (total == 0U || points == 0U) return 0U;
  if (points == 1U) return static_cast<uint16_t>(total - 1U);
  return static_cast<uint16_t>(
      (static_cast<uint32_t>(pointIndex) * (total - 1U)) / (points - 1U));
}

}  // namespace gathra::history_query
