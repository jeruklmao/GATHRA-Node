#include <unity.h>

#include <cstring>

#include "filter.hpp"
#include "node_config.hpp"
#include "protocol.hpp"
#include "retry_policy.hpp"
#include "rtc_history.hpp"

using namespace gathra;

namespace {

NodeConfig defaults() {
  NodeConfig config;
  config.setDefaults("GTH-TEST");
  return config;
}

void seed(TemporalFilter& filter, const NodeConfig& config, uint32_t value,
          uint8_t count = 7) {
  for (uint8_t i = 0; i < count; ++i) {
    const FilterResult result = filter.evaluate(value, true, config);
    TEST_ASSERT_TRUE(result.disposition == FilterDisposition::kAccepted);
  }
}

void test_protocol_telemetry_golden_big_endian() {
  protocol::TelemetryPacket source{};
  std::strcpy(source.nodeId, "N1");
  source.bootSessionId = 0x01020304U;
  source.sequence = 0xA0B0C0D0U;
  source.medianEchoUs = 0x00001234U;
  source.rawDistanceMm = 740U;
  source.acceptedDistanceMm = 739U;
  source.madMm = 3U;
  source.temperatureCentiC = -1234;
  source.humidityCentiPercent = 4567U;
  source.batteryMv = 3700U;
  source.validSamples = 7U;
  source.totalSamples = 7U;
  source.filterState = FilterState::kStable;
  source.qualityFlags = 0x0003U;
  source.healthFlags = 0x0202U;
  const uint8_t golden[] = {
      0x47, 0x54, 0x01, 0x01, 0x02, 0x4E, 0x31,
      0x01, 0x02, 0x03, 0x04, 0xA0, 0xB0, 0xC0, 0xD0,
      0x00, 0x00, 0x12, 0x34, 0x00, 0x00, 0x02, 0xE4,
      0x00, 0x00, 0x02, 0xE3, 0x00, 0x03, 0xFB, 0x2E,
      0x11, 0xD7, 0x0E, 0x74, 0x07, 0x07, 0x00, 0x00,
      0x03, 0x02, 0x02};
  uint8_t encoded[96]{};
  size_t written = 0;
  TEST_ASSERT_TRUE(protocol::encodeTelemetry(source, encoded, sizeof(encoded), written));
  TEST_ASSERT_EQUAL_UINT(sizeof(golden), written);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(golden, encoded, sizeof(golden));

  protocol::TelemetryPacket decoded{};
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
                          static_cast<uint8_t>(protocol::decodeTelemetry(encoded, written, decoded)));
  TEST_ASSERT_EQUAL_STRING(source.nodeId, decoded.nodeId);
  TEST_ASSERT_EQUAL_HEX32(source.bootSessionId, decoded.bootSessionId);
  TEST_ASSERT_EQUAL_HEX32(source.sequence, decoded.sequence);
  TEST_ASSERT_EQUAL_INT16(source.temperatureCentiC, decoded.temperatureCentiC);
  TEST_ASSERT_EQUAL_UINT16(source.healthFlags, decoded.healthFlags);
}

void test_protocol_rejects_invalid_version_type_and_trailing_data() {
  protocol::AckPacket ack{};
  std::strcpy(ack.nodeId, "N1");
  ack.bootSessionId = 1U;
  ack.sequence = 2U;
  uint8_t bytes[32]{};
  size_t written = 0;
  TEST_ASSERT_TRUE(protocol::encodeAck(ack, bytes, sizeof(bytes), written));
  protocol::AckPacket decoded{};
  bytes[2] = 2U;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kUnsupportedVersion),
                          static_cast<uint8_t>(protocol::decodeAck(bytes, written, decoded)));
  bytes[2] = 1U;
  bytes[3] = static_cast<uint8_t>(protocol::MessageType::kTelemetry);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kWrongType),
                          static_cast<uint8_t>(protocol::decodeAck(bytes, written, decoded)));
  bytes[3] = static_cast<uint8_t>(protocol::MessageType::kAck);
  bytes[written] = 0;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kTrailingData),
                          static_cast<uint8_t>(protocol::decodeAck(bytes, written + 1U, decoded)));
}

void test_ack_codec_and_matching() {
  protocol::TelemetryPacket telemetry{};
  std::strcpy(telemetry.nodeId, "GTH-ABC");
  telemetry.bootSessionId = 0x11223344U;
  telemetry.sequence = 77U;
  protocol::AckPacket ack{};
  std::strcpy(ack.nodeId, telemetry.nodeId);
  ack.bootSessionId = telemetry.bootSessionId;
  ack.sequence = telemetry.sequence;
  uint8_t bytes[64]{};
  size_t written = 0;
  TEST_ASSERT_TRUE(protocol::encodeAck(ack, bytes, sizeof(bytes), written));
  protocol::AckPacket decoded{};
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
                          static_cast<uint8_t>(protocol::decodeAck(bytes, written, decoded)));
  TEST_ASSERT_TRUE(protocol::ackMatches(decoded, telemetry));
  ++decoded.sequence;
  TEST_ASSERT_FALSE(protocol::ackMatches(decoded, telemetry));
  decoded = ack;
  ++decoded.bootSessionId;
  TEST_ASSERT_FALSE(protocol::ackMatches(decoded, telemetry));
  decoded = ack;
  std::strcpy(decoded.nodeId, "WRONG");
  TEST_ASSERT_FALSE(protocol::ackMatches(decoded, telemetry));
}

void test_retry_policy_is_bounded_to_initial_plus_retries() {
  RetryPolicy policy(2U);
  TEST_ASSERT_TRUE(policy.beginAttempt());
  TEST_ASSERT_TRUE(policy.beginAttempt());
  TEST_ASSERT_TRUE(policy.beginAttempt());
  TEST_ASSERT_TRUE(policy.exhausted());
  TEST_ASSERT_FALSE(policy.beginAttempt());
  TEST_ASSERT_EQUAL_UINT8(3U, policy.attempts());

  RetryPolicy acknowledged(2U);
  TEST_ASSERT_TRUE(acknowledged.beginAttempt());
  acknowledged.acknowledge();
  TEST_ASSERT_FALSE(acknowledged.beginAttempt());
  TEST_ASSERT_TRUE(acknowledged.acknowledged());
}

void test_burst_median_mad_rejects_single_outlier() {
  const uint32_t values[] = {740, 741, 739, 1055, 738, 740, 742};
  const RobustStats stats = robustStats(values, 7U);
  TEST_ASSERT_TRUE(stats.valid);
  TEST_ASSERT_EQUAL_UINT32(740U, stats.median);
  TEST_ASSERT_EQUAL_UINT32(1U, stats.mad);
}

void test_temporary_object_is_rejected_and_baseline_preserved() {
  NodeConfig config = defaults();
  FilterMemory memory{};
  TemporalFilter filter(memory);
  seed(filter, config, 1500U);
  FilterResult result = filter.evaluate(550U, true, config);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FilterDisposition::kNeedsVerification),
                          static_cast<uint8_t>(result.disposition));
  result = filter.observeVerification(560U, true, config);
  result = filter.observeVerification(1490U, true, config);
  result = filter.observeVerification(1500U, true, config);
  result = filter.observeVerification(1502U, true, config);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FilterState::kTransientRejected),
                          static_cast<uint8_t>(result.state));
  TEST_ASSERT_EQUAL_UINT32(1500U, result.acceptedDistanceMm);
}

void test_persistent_rapid_rise_is_confirmed() {
  NodeConfig config = defaults();
  FilterMemory memory{};
  TemporalFilter filter(memory);
  seed(filter, config, 1500U);
  FilterResult result = filter.evaluate(750U, true, config);
  result = filter.observeVerification(740U, true, config);
  result = filter.observeVerification(730U, true, config);
  result = filter.observeVerification(725U, true, config);
  result = filter.observeVerification(720U, true, config);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FilterState::kChangeConfirmed),
                          static_cast<uint8_t>(result.state));
  TEST_ASSERT_EQUAL_UINT32(730U, result.acceptedDistanceMm);
  TEST_ASSERT_TRUE(result.scheduleSoon);
  result = filter.evaluate(720U, true, config);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FilterState::kStable),
                          static_cast<uint8_t>(result.state));
}

void test_persistent_fall_uses_fall_policy_and_confirms() {
  NodeConfig config = defaults();
  config.fallVerificationCount = 5;
  config.fallRequiredConfirmations = 4;
  FilterMemory memory{};
  TemporalFilter filter(memory);
  seed(filter, config, 750U);
  FilterResult result = filter.evaluate(1500U, true, config);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FilterState::kVerifyFall),
                          static_cast<uint8_t>(result.state));
  TEST_ASSERT_EQUAL_UINT16(5000U, filter.verificationIntervalMs(config));
  result = filter.observeVerification(1490U, true, config);
  result = filter.observeVerification(1510U, true, config);
  result = filter.observeVerification(1480U, true, config);
  result = filter.observeVerification(1505U, true, config);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FilterState::kChangeConfirmed),
                          static_cast<uint8_t>(result.state));
}

void test_mad_zero_uses_absolute_floor_and_invalid_preserves_state() {
  NodeConfig config = defaults();
  FilterMemory memory{};
  TemporalFilter filter(memory);
  seed(filter, config, 1000U);
  FilterResult result = filter.evaluate(1060U, true, config);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FilterDisposition::kNeedsVerification),
                          static_cast<uint8_t>(result.disposition));
  result = filter.finishVerification(config);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FilterState::kUncertain),
                          static_cast<uint8_t>(result.state));
  result = filter.evaluate(kDistanceUnavailable, false, config);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FilterState::kInvalid),
                          static_cast<uint8_t>(result.state));
  TEST_ASSERT_NOT_EQUAL(kDistanceUnavailable, result.acceptedDistanceMm);
}

void test_config_validation_rejects_unsafe_ranges() {
  NodeConfig config = defaults();
  TEST_ASSERT_TRUE(static_cast<bool>(validateConfig(config)));
  config.loraSpreadingFactor = 13;
  TEST_ASSERT_FALSE(static_cast<bool>(validateConfig(config)));
  config = defaults(); config.loraCodingRateDenominator = 4;
  TEST_ASSERT_FALSE(static_cast<bool>(validateConfig(config)));
  config = defaults(); config.loraFrequencyMhz = 600.0F;
  TEST_ASSERT_FALSE(static_cast<bool>(validateConfig(config)));
  config = defaults(); config.emaAlpha = 0.0F;
  TEST_ASSERT_FALSE(static_cast<bool>(validateConfig(config)));
  config = defaults(); config.riseRequiredConfirmations = 0;
  TEST_ASSERT_FALSE(static_cast<bool>(validateConfig(config)));
  config = defaults(); config.sonarMinimumValid = config.sonarBurstCount + 1U;
  TEST_ASSERT_FALSE(static_cast<bool>(validateConfig(config)));
  config = defaults(); config.changingWakeIntervalSec = config.normalWakeIntervalSec;
  TEST_ASSERT_FALSE(static_cast<bool>(validateConfig(config)));
  config = defaults();
  config.riseVerificationIntervalMs = config.fallVerificationIntervalMs + 1U;
  TEST_ASSERT_FALSE(static_cast<bool>(validateConfig(config)));
}

void test_ring_empty_fill_wrap_order_and_version_reset() {
  RtcRetainedState state{};
  initializeRtcState(state, 123U);
  TEST_ASSERT_TRUE(rtcStateValid(state));
  HistoryEntry output{};
  TEST_ASSERT_FALSE(historyAt(state, 0U, output));
  for (uint32_t sequence = 1; sequence <= build::kHistoryCapacity + 5U; ++sequence) {
    HistoryEntry entry{};
    entry.sequence = sequence;
    appendHistory(state, entry);
  }
  TEST_ASSERT_EQUAL_UINT8(build::kHistoryCapacity, state.historyCount);
  TEST_ASSERT_TRUE(historyAt(state, 0U, output));
  TEST_ASSERT_EQUAL_UINT32(6U, output.sequence);
  TEST_ASSERT_TRUE(historyAt(state, build::kHistoryCapacity - 1U, output));
  TEST_ASSERT_EQUAL_UINT32(build::kHistoryCapacity + 5U, output.sequence);
  state.schemaVersion = 99U;
  TEST_ASSERT_FALSE(rtcStateValid(state));
  initializeRtcState(state, 456U);
  TEST_ASSERT_EQUAL_UINT8(0U, state.historyCount);
  TEST_ASSERT_EQUAL_UINT32(456U, state.bootSessionId);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_protocol_telemetry_golden_big_endian);
  RUN_TEST(test_protocol_rejects_invalid_version_type_and_trailing_data);
  RUN_TEST(test_ack_codec_and_matching);
  RUN_TEST(test_retry_policy_is_bounded_to_initial_plus_retries);
  RUN_TEST(test_burst_median_mad_rejects_single_outlier);
  RUN_TEST(test_temporary_object_is_rejected_and_baseline_preserved);
  RUN_TEST(test_persistent_rapid_rise_is_confirmed);
  RUN_TEST(test_persistent_fall_uses_fall_policy_and_confirms);
  RUN_TEST(test_mad_zero_uses_absolute_floor_and_invalid_preserves_state);
  RUN_TEST(test_config_validation_rejects_unsafe_ranges);
  RUN_TEST(test_ring_empty_fill_wrap_order_and_version_reset);
  return UNITY_END();
}
