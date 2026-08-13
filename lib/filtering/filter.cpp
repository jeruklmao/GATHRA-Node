#include "filter.hpp"

#include <math.h>
#include <string.h>

namespace gathra {
namespace {

void sortAscending(uint32_t* values, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const uint32_t current = values[i];
    size_t j = i;
    while (j > 0U && values[j - 1U] > current) {
      values[j] = values[j - 1U];
      --j;
    }
    values[j] = current;
  }
}

uint32_t absoluteDifference(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

uint32_t medianSorted(const uint32_t* values, size_t count) {
  const size_t middle = count / 2U;
  if ((count % 2U) != 0U) return values[middle];
  return values[middle - 1U] + (values[middle] - values[middle - 1U]) / 2U;
}

}  // namespace

RobustStats robustStats(const uint32_t* values, size_t count) {
  RobustStats result{};
  if (values == nullptr || count == 0U || count > build::kMaximumSonarSamples) return result;
  uint32_t sorted[build::kMaximumSonarSamples]{};
  memcpy(sorted, values, count * sizeof(uint32_t));
  sortAscending(sorted, count);
  result.median = medianSorted(sorted, count);
  uint32_t deviations[build::kMaximumSonarSamples]{};
  for (size_t i = 0; i < count; ++i) {
    deviations[i] = absoluteDifference(values[i], result.median);
  }
  sortAscending(deviations, count);
  result.mad = medianSorted(deviations, count);
  result.valid = true;
  return result;
}

uint32_t TemporalFilter::historyValue(uint8_t chronologicalIndex) const {
  const uint8_t capacity = build::kMaximumHampelWindow;
  const uint8_t oldest = memory_.historyCount < capacity ? 0U : memory_.historyHead;
  return memory_.acceptedHistory[(oldest + chronologicalIndex) % capacity];
}

void TemporalFilter::appendAccepted(uint32_t value) {
  memory_.acceptedHistory[memory_.historyHead] = value;
  memory_.historyHead = (memory_.historyHead + 1U) % build::kMaximumHampelWindow;
  if (memory_.historyCount < build::kMaximumHampelWindow) ++memory_.historyCount;
}

FilterResult TemporalFilter::accept(uint32_t value, FilterState state,
                                    const NodeConfig& config, bool resetEma) {
  if (!memory_.hasAccepted || resetEma) {
    memory_.emaDistanceMm = static_cast<float>(value);
    memory_.historyCount = 0;
    memory_.historyHead = 0;
  } else {
    memory_.emaDistanceMm = config.emaAlpha * static_cast<float>(value) +
                            (1.0F - config.emaAlpha) * memory_.emaDistanceMm;
  }
  memory_.hasAccepted = true;
  memory_.lastAcceptedMm = static_cast<uint32_t>(lroundf(memory_.emaDistanceMm));
  appendAccepted(memory_.lastAcceptedMm);
  memory_.lastState = state;
  return {state == FilterState::kChangeConfirmed ? FilterDisposition::kChangeConfirmed
                                                 : FilterDisposition::kAccepted,
          state,
          memory_.lastAcceptedMm,
          memory_.lastCandidateMm,
          state == FilterState::kChangeConfirmed};
}

void TemporalFilter::beginCandidate(uint32_t rawDistanceMm) {
  memory_.candidateActive = true;
  memory_.candidateIsRise = rawDistanceMm < memory_.lastAcceptedMm;
  memory_.candidateMm = rawDistanceMm;
  memory_.candidateSamples[0] = rawDistanceMm;
  memory_.candidateSampleCount = 1;
  memory_.candidateObservations = 1;
  memory_.candidateVotes = 1;
  memory_.baselineVotes = 0;
  memory_.lastCandidateMm = rawDistanceMm;
  memory_.lastState = memory_.candidateIsRise ? FilterState::kVerifyRise
                                              : FilterState::kVerifyFall;
}

FilterResult TemporalFilter::evaluate(uint32_t rawDistanceMm, bool valid,
                                      const NodeConfig& config) {
  if (!valid || rawDistanceMm == kDistanceUnavailable) {
    memory_.lastState = FilterState::kInvalid;
    return {FilterDisposition::kInvalid, FilterState::kInvalid,
            memory_.hasAccepted ? memory_.lastAcceptedMm : kDistanceUnavailable,
            memory_.lastCandidateMm, true};
  }
  if (!memory_.hasAccepted || memory_.historyCount == 0U) {
    return accept(rawDistanceMm, FilterState::kAccepted, config, true);
  }

  uint32_t recent[build::kMaximumHampelWindow]{};
  const uint8_t useCount = memory_.historyCount < config.hampelWindow
                               ? memory_.historyCount
                               : config.hampelWindow;
  const uint8_t skip = memory_.historyCount - useCount;
  for (uint8_t i = 0; i < useCount; ++i) recent[i] = historyValue(skip + i);
  const RobustStats stats = robustStats(recent, useCount);
  const float scaledMad = 1.4826F * static_cast<float>(stats.mad);
  const uint32_t statisticalThreshold = static_cast<uint32_t>(
      lroundf(config.hampelMultiplier * scaledMad));
  const uint32_t hampelThreshold = statisticalThreshold > config.hampelAbsoluteFloorMm
                                       ? statisticalThreshold
                                       : config.hampelAbsoluteFloorMm;
  const bool hampelOutlier =
      stats.valid && absoluteDifference(rawDistanceMm, stats.median) > hampelThreshold;
  const bool sudden = absoluteDifference(rawDistanceMm, memory_.lastAcceptedMm) >=
                      config.suddenChangeThresholdMm;
  if (hampelOutlier || sudden) {
    beginCandidate(rawDistanceMm);
    return {FilterDisposition::kNeedsVerification, memory_.lastState,
            memory_.lastAcceptedMm, rawDistanceMm, true};
  }
  return accept(rawDistanceMm, FilterState::kStable, config);
}

uint8_t TemporalFilter::verificationTarget(const NodeConfig& config) const {
  return memory_.candidateIsRise ? config.riseVerificationCount
                                 : config.fallVerificationCount;
}

uint16_t TemporalFilter::verificationIntervalMs(const NodeConfig& config) const {
  return memory_.candidateIsRise ? config.riseVerificationIntervalMs
                                 : config.fallVerificationIntervalMs;
}

FilterResult TemporalFilter::observeVerification(uint32_t rawDistanceMm, bool valid,
                                                 const NodeConfig& config) {
  if (!memory_.candidateActive) {
    return {FilterDisposition::kInvalid, FilterState::kInvalid,
            memory_.hasAccepted ? memory_.lastAcceptedMm : kDistanceUnavailable,
            memory_.lastCandidateMm, true};
  }
  ++memory_.candidateObservations;
  if (valid && rawDistanceMm != kDistanceUnavailable) {
    const uint16_t tolerance = memory_.candidateIsRise ? config.riseToleranceMm
                                                        : config.fallToleranceMm;
    if (absoluteDifference(rawDistanceMm, memory_.candidateMm) <= tolerance) {
      ++memory_.candidateVotes;
      if (memory_.candidateSampleCount < build::kMaximumSonarSamples) {
        memory_.candidateSamples[memory_.candidateSampleCount++] = rawDistanceMm;
      }
    }
    if (absoluteDifference(rawDistanceMm, memory_.lastAcceptedMm) <= tolerance) {
      ++memory_.baselineVotes;
    }
  }
  if (memory_.candidateObservations >= verificationTarget(config)) {
    return finishVerification(config);
  }
  return {FilterDisposition::kPending, memory_.lastState, memory_.lastAcceptedMm,
          memory_.candidateMm, true};
}

FilterResult TemporalFilter::finishVerification(const NodeConfig& config) {
  if (!memory_.candidateActive) {
    return {FilterDisposition::kInvalid, FilterState::kInvalid,
            memory_.hasAccepted ? memory_.lastAcceptedMm : kDistanceUnavailable,
            memory_.lastCandidateMm, true};
  }
  const uint8_t required = memory_.candidateIsRise ? config.riseRequiredConfirmations
                                                    : config.fallRequiredConfirmations;
  const uint8_t majority = memory_.candidateObservations / 2U + 1U;
  const uint8_t observations = memory_.candidateObservations;
  const uint8_t votes = memory_.candidateVotes;
  const uint32_t candidate = memory_.candidateMm;
  memory_.lastCandidateMm = candidate;
  memory_.lastCandidateObservations = observations;
  memory_.lastCandidateVotes = votes;
  memory_.candidateActive = false;

  if (votes >= required && memory_.candidateSampleCount > 0U) {
    const RobustStats stats = robustStats(memory_.candidateSamples, memory_.candidateSampleCount);
    return accept(stats.valid ? stats.median : candidate, FilterState::kChangeConfirmed,
                  config, true);
  }
  if (memory_.baselineVotes >= majority) {
    memory_.lastState = FilterState::kTransientRejected;
    return {FilterDisposition::kTransientRejected, FilterState::kTransientRejected,
            memory_.lastAcceptedMm, candidate, false};
  }
  memory_.lastState = FilterState::kUncertain;
  return {FilterDisposition::kUncertain, FilterState::kUncertain,
          memory_.lastAcceptedMm, candidate, true};
}

}  // namespace gathra
