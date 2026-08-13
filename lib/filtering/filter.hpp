#pragma once

#include <stddef.h>
#include <stdint.h>

#include "build_config.hpp"
#include "model.hpp"
#include "node_config.hpp"

namespace gathra {

struct RobustStats {
  bool valid = false;
  uint32_t median = 0;
  uint32_t mad = 0;
};

RobustStats robustStats(const uint32_t* values, size_t count);

struct FilterMemory {
  uint32_t acceptedHistory[build::kMaximumHampelWindow]{};
  uint8_t historyCount = 0;
  uint8_t historyHead = 0;
  bool hasAccepted = false;
  float emaDistanceMm = 0.0F;
  uint32_t lastAcceptedMm = kDistanceUnavailable;

  bool candidateActive = false;
  bool candidateIsRise = false;
  uint32_t candidateMm = kDistanceUnavailable;
  uint32_t candidateSamples[build::kMaximumSonarSamples]{};
  uint8_t candidateSampleCount = 0;
  uint8_t candidateObservations = 0;
  uint8_t candidateVotes = 0;
  uint8_t baselineVotes = 0;

  uint32_t lastCandidateMm = kDistanceUnavailable;
  uint8_t lastCandidateObservations = 0;
  uint8_t lastCandidateVotes = 0;
  FilterState lastState = FilterState::kInvalid;
};

enum class FilterDisposition : uint8_t {
  kAccepted,
  kNeedsVerification,
  kPending,
  kTransientRejected,
  kChangeConfirmed,
  kUncertain,
  kInvalid,
};

struct FilterResult {
  FilterDisposition disposition = FilterDisposition::kInvalid;
  FilterState state = FilterState::kInvalid;
  uint32_t acceptedDistanceMm = kDistanceUnavailable;
  uint32_t candidateDistanceMm = kDistanceUnavailable;
  bool scheduleSoon = true;
};

class TemporalFilter {
 public:
  explicit TemporalFilter(FilterMemory& memory) : memory_(memory) {}

  FilterResult evaluate(uint32_t rawDistanceMm, bool valid, const NodeConfig& config);
  FilterResult observeVerification(uint32_t rawDistanceMm, bool valid,
                                   const NodeConfig& config);
  FilterResult finishVerification(const NodeConfig& config);
  uint8_t verificationTarget(const NodeConfig& config) const;
  uint16_t verificationIntervalMs(const NodeConfig& config) const;
  bool candidateActive() const { return memory_.candidateActive; }

 private:
  FilterResult accept(uint32_t value, FilterState state, const NodeConfig& config,
                      bool resetEma = false);
  void appendAccepted(uint32_t value);
  void beginCandidate(uint32_t rawDistanceMm);
  uint32_t historyValue(uint8_t chronologicalIndex) const;

  FilterMemory& memory_;
};

}  // namespace gathra
