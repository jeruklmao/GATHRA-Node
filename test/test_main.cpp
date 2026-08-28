#include <unity.h>

#include <array>
#include <cstring>
#include <vector>

#include "command_processor.hpp"
#include "filter.hpp"
#include "history_query.hpp"
#include "legacy_config_migration.hpp"
#include "node_config.hpp"
#include "nvs_history.hpp"
#include "pcf8563.hpp"
#include "persistent_state.hpp"
#include "power_manager.hpp"
#include "protocol.hpp"
#include "retry_policy.hpp"

using namespace gathra;

namespace {

NodeConfig defaults() {
  NodeConfig config;
  config.setDefaults("GTH-TEST");
  return config;
}

class FakeRegisterIo final : public rtc::RegisterIo {
 public:
  bool read(uint8_t first, uint8_t* output, size_t length) override {
    if (failRead || first + length > registers.size()) return false;
    std::memcpy(output, registers.data() + first, length);
    return true;
  }
  bool write(uint8_t first, const uint8_t* input, size_t length) override {
    if (failWrite || first + length > registers.size()) return false;
    for (size_t i = 0; i < length; ++i) {
      const uint8_t address = static_cast<uint8_t>(first + i);
      if (address == rtc::kRegControl2) {
        const uint8_t flags = static_cast<uint8_t>(
            registers[address] & input[i] & (rtc::kControl2Tf | rtc::kControl2Af));
        registers[address] = static_cast<uint8_t>(
            flags | (input[i] & (rtc::kControl2TiTp | rtc::kControl2Tie |
                                 rtc::kControl2Aie)));
      } else {
        registers[address] = input[i];
      }
    }
    return true;
  }
  std::array<uint8_t, 16> registers{};
  bool failRead = false;
  bool failWrite = false;
};

class FakeStateBackend final : public StateBackend {
 public:
  bool load(PersistentState& output) override {
    if (!present) return false;
    output = state;
    return true;
  }
  bool save(const PersistentState& input) override {
    ++saveCalls;
    if (failSaves > 0) {
      --failSaves;
      return false;
    }
    state = input;
    present = true;
    return true;
  }
  PersistentState state{};
  bool present = false;
  int failSaves = 0;
  int saveCalls = 0;
};

class FakeHistoryBackend final : public HistoryBackend {
 public:
  bool begin() override { return beginOkay; }
  bool readMetadata(uint8_t copy, HistoryMetadata& output) override {
    if (copy > 1U || !metadataPresent[copy]) return false;
    output = metadata[copy];
    return true;
  }
  bool writeMetadata(uint8_t copy, const HistoryMetadata& input) override {
    if (copy > 1U || failMetadataWrite) return false;
    metadata[copy] = input;
    metadataPresent[copy] = true;
    return true;
  }
  bool readSlot(uint16_t slot, HistoryRecord& output) override {
    if (slot >= build::kHistoryCapacity || !slotPresent[slot]) return false;
    output = slots[slot];
    return true;
  }
  bool writeSlot(uint16_t slot, const HistoryRecord& input) override {
    if (slot >= build::kHistoryCapacity || failSlotWrite) return false;
    slots[slot] = input;
    slotPresent[slot] = true;
    return true;
  }
  bool beginOkay = true;
  bool failMetadataWrite = false;
  bool failSlotWrite = false;
  HistoryMetadata metadata[2]{};
  bool metadataPresent[2]{};
  std::array<HistoryRecord, build::kHistoryCapacity> slots{};
  std::array<bool, build::kHistoryCapacity> slotPresent{};
};

class FakeCommandEnvironment final : public CommandEnvironment {
 public:
  const StoredCommand& storedCommand() const override { return stored; }
  bool persistReceipt(const StoredCommand& command) override {
    ++receiptWrites;
    if (!storageOkay) return false;
    stored = command;
    return true;
  }
  protocol::CommandResultCode applyEnterMaintenance() override {
    ++enterApplications;
    return protocol::CommandResultCode::kApplied;
  }
  protocol::CommandResultCode applyPollInterval(uint8_t minutes) override {
    ++pollApplications;
    poll = minutes;
    return protocol::CommandResultCode::kApplied;
  }
  protocol::CommandResultCode applyMaintenanceSchedule(uint32_t target) override {
    ++scheduleApplications;
    schedule = target;
    return scheduleResult;
  }
  bool persistResult(const StoredCommand& command) override {
    ++resultWrites;
    if (!storageOkay) return false;
    stored = command;
    return true;
  }
  uint8_t effectivePollIntervalMinutes() const override { return poll; }
  uint32_t effectiveMaintenanceUnix() const override { return schedule; }

  StoredCommand stored{};
  bool storageOkay = true;
  uint8_t poll = 10;
  uint32_t schedule = 0;
  protocol::CommandResultCode scheduleResult = protocol::CommandResultCode::kApplied;
  int receiptWrites = 0;
  int resultWrites = 0;
  int enterApplications = 0;
  int pollApplications = 0;
  int scheduleApplications = 0;
};

protocol::TelemetryPacket canonicalTelemetry() {
  protocol::TelemetryPacket p{};
  std::strcpy(p.nodeId, "N1");
  p.persistentSessionId = 0x01020304U;
  p.sequence = 0xA0B0C0D0U;
  p.medianEchoUs = 0x00001234U;
  p.rawDistanceMm = 740U;
  p.acceptedDistanceMm = 739U;
  p.madMm = 3U;
  p.temperatureCentiC = -1234;
  p.humidityCentiPercent = 4567U;
  p.batteryMv = 3700U;
  p.validSamples = 7U;
  p.totalSamples = 7U;
  p.filterState = FilterState::kStable;
  p.qualityFlags = 0x0003U;
  p.healthFlags = 0x0202U;
  p.bootReason = protocol::BootReason::kRtcTimer;
  p.rtcState = protocol::RtcState::kValid;
  p.rtcUnixTime = 0x69ABCDEFU;
  p.pollIntervalMinutes = 10U;
  p.scheduleState = protocol::ScheduleState::kPending;
  p.scheduledMaintenanceUnix = 0x69ABF000U;
  p.lastCommandId = 0x01020305U;
  p.lastCommandType = protocol::CommandType::kSetPollIntervalMinutes;
  p.lastCommandResult = protocol::CommandResultCode::kApplied;
  p.referenceDistanceMm = 1500U;
  return p;
}

void test_protocol_v3_telemetry_golden_big_endian() {
  const protocol::TelemetryPacket source = canonicalTelemetry();
  const uint8_t golden[] = {
      0x47,0x54,0x03,0x01,0x02,0x4E,0x31,
      0x01,0x02,0x03,0x04,0xA0,0xB0,0xC0,0xD0,
      0x00,0x00,0x12,0x34,0x00,0x00,0x02,0xE4,
      0x00,0x00,0x02,0xE3,0x00,0x03,0xFB,0x2E,
      0x11,0xD7,0x0E,0x74,0x07,0x07,0x00,0x00,
      0x03,0x02,0x02,0x00,0x00,0x69,0xAB,0xCD,
      0xEF,0x0A,0x01,0x69,0xAB,0xF0,0x00,0x01,
      0x02,0x03,0x05,0x03,0x00,0x00,0x00,0x05,
      0xDC};
  uint8_t bytes[96]{};
  size_t written = 0;
  TEST_ASSERT_TRUE(protocol::encodeTelemetry(source, bytes, sizeof(bytes), written));
  TEST_ASSERT_EQUAL_UINT(53U, protocol::kReferenceDistancePayloadOffset);
  TEST_ASSERT_EQUAL_UINT(57U, protocol::kTelemetryPayloadBytes);
  TEST_ASSERT_EQUAL_UINT(62U + std::strlen(source.nodeId), written);
  TEST_ASSERT_EQUAL_UINT(sizeof(golden), written);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(golden, bytes, sizeof(golden));
  TEST_ASSERT_EQUAL_HEX8(0x00U, bytes[written - 4U]);
  TEST_ASSERT_EQUAL_HEX8(0x00U, bytes[written - 3U]);
  TEST_ASSERT_EQUAL_HEX8(0x05U, bytes[written - 2U]);
  TEST_ASSERT_EQUAL_HEX8(0xDCU, bytes[written - 1U]);
  protocol::TelemetryPacket decoded{};
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes, written, decoded)));
  TEST_ASSERT_EQUAL_HEX32(source.persistentSessionId, decoded.persistentSessionId);
  TEST_ASSERT_EQUAL_HEX32(source.rtcUnixTime, decoded.rtcUnixTime);
  TEST_ASSERT_EQUAL_UINT8(10U, decoded.pollIntervalMinutes);
  TEST_ASSERT_EQUAL_HEX32(source.lastCommandId, decoded.lastCommandId);
  TEST_ASSERT_EQUAL_UINT32(1500U, decoded.referenceDistanceMm);
}

void test_protocol_v3_reference_sentinels_and_malformed_packets() {
  auto p = canonicalTelemetry();
  p.rawDistanceMm = kDistanceUnavailable;
  p.acceptedDistanceMm = kDistanceUnavailable;
  p.temperatureCentiC = kTemperatureUnavailable;
  p.humidityCentiPercent = kHumidityUnavailable;
  p.rtcState = protocol::RtcState::kInvalidVl;
  p.rtcUnixTime = 0U;
  p.referenceDistanceMm = 0U;
  uint8_t bytes[96]{};
  size_t written = 0;
  TEST_ASSERT_TRUE(protocol::encodeTelemetry(p, bytes, sizeof(bytes), written));
  protocol::TelemetryPacket decoded{};
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes, written, decoded)));
  TEST_ASSERT_EQUAL_UINT32(0U, decoded.referenceDistanceMm);

  p.referenceDistanceMm = UINT32_MAX;
  TEST_ASSERT_TRUE(protocol::encodeTelemetry(p, bytes, sizeof(bytes), written));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes, written, decoded)));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, decoded.referenceDistanceMm);

  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kBufferTooSmall),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes, written - 1U, decoded)));
  bytes[2] = 1U;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kUnsupportedVersion),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes, written, decoded)));
  bytes[2] = 2U;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kUnsupportedVersion),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes, written - 4U, decoded)));
  bytes[2] = 3U;
  bytes[written] = 0U;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kTrailingData),
      static_cast<uint8_t>(protocol::decodeTelemetry(bytes, written + 1U, decoded)));
}

void test_ack_command_none_time_flags_and_all_commands() {
  const auto telemetry = canonicalTelemetry();
  protocol::AckCommandPacket ack{};
  std::strcpy(ack.nodeId, telemetry.nodeId);
  ack.persistentSessionId = telemetry.persistentSessionId;
  ack.sequence = telemetry.sequence;
  uint8_t bytes[64]{};
  size_t written = 0;
  TEST_ASSERT_TRUE(protocol::encodeAckCommand(ack, bytes, sizeof(bytes), written));
  TEST_ASSERT_EQUAL_UINT(protocol::kCommonHeaderFixedBytes +
                             std::strlen(ack.nodeId) +
                             protocol::kAckCommandFixedPayloadBytes,
                         written);
  TEST_ASSERT_EQUAL_UINT8(protocol::kVersion, bytes[2]);
  protocol::AckCommandPacket decoded{};
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeAckCommand(bytes, written, decoded)));
  TEST_ASSERT_FALSE(decoded.timeValid);
  TEST_ASSERT_TRUE(protocol::ackMatches(decoded, telemetry));
  bytes[2] = 2U;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kUnsupportedVersion),
      static_cast<uint8_t>(protocol::decodeAckCommand(bytes, written, decoded)));
  bytes[2] = protocol::kVersion;

  ack.timeValid = true;
  ack.gatewayUnixTime = 1787600000U;
  ack.commandId = 1U;
  ack.commandType = protocol::CommandType::kEnterMaintenanceNow;
  TEST_ASSERT_TRUE(protocol::encodeAckCommand(ack, bytes, sizeof(bytes), written));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeAckCommand(bytes, written, decoded)));
  TEST_ASSERT_TRUE(decoded.timeValid);

  ack.commandId = 2U;
  ack.commandType = protocol::CommandType::kSetPollIntervalMinutes;
  ack.pollIntervalMinutes = 5U;
  TEST_ASSERT_TRUE(protocol::encodeAckCommand(ack, bytes, sizeof(bytes), written));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeAckCommand(bytes, written, decoded)));
  TEST_ASSERT_EQUAL_UINT8(5U, decoded.pollIntervalMinutes);

  ack.commandId = 3U;
  ack.commandType = protocol::CommandType::kScheduleMaintenanceAt;
  ack.scheduledMaintenanceUnix = 1787600400U;
  TEST_ASSERT_TRUE(protocol::encodeAckCommand(ack, bytes, sizeof(bytes), written));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
      static_cast<uint8_t>(protocol::decodeAckCommand(bytes, written, decoded)));
  bytes[written - 5U] = 3U;  // Corrupt payload length (expected four).
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kInvalidCommand),
      static_cast<uint8_t>(protocol::decodeAckCommand(bytes, written, decoded)));
}

void test_command_result_codec_all_result_codes() {
  for (uint8_t code = 0; code <= 7U; ++code) {
    protocol::CommandResultPacket source{};
    std::strcpy(source.nodeId, "N1");
    source.persistentSessionId = 0x11223344U;
    source.commandId = 0xAABBCCDDU;
    source.commandType = protocol::CommandType::kScheduleMaintenanceAt;
    source.resultCode = static_cast<protocol::CommandResultCode>(code);
    source.effectivePollIntervalMinutes = 10U;
    source.scheduledMaintenanceUnix = 1787600400U;
    uint8_t bytes[64]{};
    size_t written = 0;
    TEST_ASSERT_TRUE(protocol::encodeCommandResult(source, bytes, sizeof(bytes), written));
    TEST_ASSERT_EQUAL_UINT(protocol::kCommonHeaderFixedBytes +
                               std::strlen(source.nodeId) +
                               protocol::kCommandResultPayloadBytes,
                           written);
    TEST_ASSERT_EQUAL_UINT8(protocol::kVersion, bytes[2]);
    protocol::CommandResultPacket decoded{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::DecodeStatus::kOk),
        static_cast<uint8_t>(protocol::decodeCommandResult(bytes, written, decoded)));
    TEST_ASSERT_EQUAL_HEX32(source.commandId, decoded.commandId);
    TEST_ASSERT_EQUAL_UINT8(code, static_cast<uint8_t>(decoded.resultCode));
  }
}

void test_pcf_bcd_time_vl_and_boundaries() {
  TEST_ASSERT_EQUAL_HEX8(0x59U, rtc::Pcf8563::bcdEncode(59U));
  uint8_t value = 0;
  TEST_ASSERT_TRUE(rtc::Pcf8563::bcdDecode(0x23U, 23U, value));
  TEST_ASSERT_EQUAL_UINT8(23U, value);
  TEST_ASSERT_FALSE(rtc::Pcf8563::bcdDecode(0x2AU, 59U, value));
  rtc::DateTime leap{2028, 2, 29, 2, 23, 59, 59};
  uint32_t unixTime = 0;
  TEST_ASSERT_TRUE(rtc::Pcf8563::dateTimeToUnix(leap, unixTime));
  rtc::DateTime roundtrip{};
  TEST_ASSERT_TRUE(rtc::Pcf8563::unixToDateTime(unixTime, roundtrip));
  TEST_ASSERT_EQUAL_UINT16(2028U, roundtrip.year);
  TEST_ASSERT_EQUAL_UINT8(29U, roundtrip.day);
  rtc::DateTime invalid{2027, 2, 29, 1, 0, 0, 0};
  TEST_ASSERT_FALSE(rtc::Pcf8563::dateTimeValid(invalid));

  FakeRegisterIo io;
  rtc::Pcf8563 pcf(io);
  TEST_ASSERT_TRUE(pcf.writeDateTime(leap));
  rtc::DateTime observed{};
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(rtc::TimeState::kValid),
      static_cast<uint8_t>(pcf.readDateTime(observed, &unixTime)));
  io.registers[rtc::kRegSeconds] |= rtc::kVoltageLow;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(rtc::TimeState::kInvalidVl),
      static_cast<uint8_t>(pcf.readDateTime(observed, &unixTime)));
  TEST_ASSERT_EQUAL_UINT32(0U, unixTime);
}

void test_pcf_timer_level_mode_and_independent_flag_clear() {
  FakeRegisterIo io;
  io.registers[rtc::kRegControl2] = rtc::kControl2Tf | rtc::kControl2Af |
                                    rtc::kControl2Tie | rtc::kControl2Aie;
  rtc::Pcf8563 pcf(io);
  rtc::TimerConfiguration timer{};
  timer.enabled = true;
  timer.source = rtc::TimerSource::kOnePerMinute;
  timer.value = 10U;
  timer.interruptEnabled = true;
  timer.levelMode = true;
  TEST_ASSERT_TRUE(pcf.configureTimer(timer));
  TEST_ASSERT_EQUAL_HEX8(0x83U, io.registers[rtc::kRegTimerControl]);
  TEST_ASSERT_EQUAL_UINT8(10U, io.registers[rtc::kRegTimer]);
  TEST_ASSERT_BITS_LOW(rtc::kControl2TiTp, io.registers[rtc::kRegControl2]);
  TEST_ASSERT_TRUE(pcf.clearFlags(true, false));
  TEST_ASSERT_BITS_LOW(rtc::kControl2Tf, io.registers[rtc::kRegControl2]);
  TEST_ASSERT_BITS_HIGH(rtc::kControl2Af, io.registers[rtc::kRegControl2]);
  io.registers[rtc::kRegControl2] |= rtc::kControl2Tf;
  TEST_ASSERT_TRUE(pcf.clearFlags(false, true));
  TEST_ASSERT_BITS_HIGH(rtc::kControl2Tf, io.registers[rtc::kRegControl2]);
  TEST_ASSERT_BITS_LOW(rtc::kControl2Af, io.registers[rtc::kRegControl2]);

  // Completing a one-shot alarm clears AF and disables AIE in the same final
  // register write while preserving TF/TIE as an independent latch/source.
  io.registers[rtc::kRegControl2] |= rtc::kControl2Af | rtc::kControl2Aie;
  TEST_ASSERT_TRUE(pcf.releaseInterruptFlags(false, true, true));
  TEST_ASSERT_BITS_HIGH(rtc::kControl2Tf | rtc::kControl2Tie,
                        io.registers[rtc::kRegControl2]);
  TEST_ASSERT_BITS_LOW(rtc::kControl2Af | rtc::kControl2Aie,
                       io.registers[rtc::kRegControl2]);

  timer.source = rtc::TimerSource::k64Hz;
  timer.value = 8U;
  TEST_ASSERT_TRUE(pcf.configureTimer(timer));
  TEST_ASSERT_EQUAL_HEX8(0x81U, io.registers[rtc::kRegTimerControl]);
}

void test_pcf_alarm_encoding_and_schedule_horizon() {
  FakeRegisterIo io;
  rtc::Pcf8563 pcf(io);
  rtc::DateTime target{2026, 8, 25, 2, 17, 42, 0};
  TEST_ASSERT_TRUE(pcf.configureAlarmForUtc(target));
  TEST_ASSERT_EQUAL_HEX8(0x42U, io.registers[0x09]);
  TEST_ASSERT_EQUAL_HEX8(0x17U, io.registers[0x0A]);
  TEST_ASSERT_EQUAL_HEX8(0x25U, io.registers[0x0B]);
  TEST_ASSERT_EQUAL_HEX8(0x80U, io.registers[0x0C]);
  TEST_ASSERT_BITS_HIGH(rtc::kControl2Aie, io.registers[rtc::kRegControl2]);
  uint32_t now = 0, future = 0;
  rtc::DateTime nowDate{2026, 8, 25, 2, 17, 0, 0};
  TEST_ASSERT_TRUE(rtc::Pcf8563::dateTimeToUnix(nowDate, now));
  TEST_ASSERT_TRUE(rtc::Pcf8563::dateTimeToUnix(target, future));
  TEST_ASSERT_TRUE(rtc::Pcf8563::scheduleRepresentable(now, future));
  TEST_ASSERT_FALSE(rtc::Pcf8563::scheduleRepresentable(now, future + 1U));
  TEST_ASSERT_FALSE(rtc::Pcf8563::scheduleRepresentable(now,
      now + 28U * 24U * 60U * 60U));
}

void test_power_transaction_preserves_active_alarm_until_final_release() {
  FakeRegisterIo io;
  io.registers[rtc::kRegControl2] = rtc::kControl2Af | rtc::kControl2Aie;
  // Model the already-matched one-shot alarm registers. They must remain live
  // while AF alone is holding external power.
  io.registers[0x09] = 0x42U;
  io.registers[0x0A] = 0x17U;
  io.registers[0x0B] = 0x25U;
  io.registers[0x0C] = rtc::kAlarmDisabled;
  rtc::Pcf8563 pcf(io);
  PowerManager power(pcf);

  TEST_ASSERT_TRUE(power.programNextWake(
      10U, protocol::ScheduleState::kCompleted, 0U));
  TEST_ASSERT_BITS_HIGH(rtc::kControl2Af | rtc::kControl2Aie,
                        io.registers[rtc::kRegControl2]);
  TEST_ASSERT_EQUAL_HEX8(0x83U, io.registers[rtc::kRegTimerControl]);
  TEST_ASSERT_EQUAL_UINT8(10U, io.registers[rtc::kRegTimer]);

  TEST_ASSERT_TRUE(power.releaseActiveFlags(false, true, true));
  TEST_ASSERT_BITS_LOW(rtc::kControl2Af | rtc::kControl2Aie,
                       io.registers[rtc::kRegControl2]);
  TEST_ASSERT_BITS_HIGH(rtc::kControl2Tie,
                        io.registers[rtc::kRegControl2]);
}

void test_boot_classification_precedence_and_alarm_target_match() {
  rtc::Status status{};
  status.communicationOkay = true;
  status.timerFlag = true;
  status.alarmFlag = true;
  const uint32_t target = 1'787'600'000U;

  auto classified = PowerManager::classify(
      status, ExpectedRebootMode::kOta, rtc::TimeState::kValid, target,
      protocol::ScheduleState::kPending, target);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::BootReason::kOtaReboot),
                          static_cast<uint8_t>(classified.reason));
  TEST_ASSERT_TRUE(classified.enterMaintenance);
  TEST_ASSERT_TRUE(classified.alarmFlagAtBoot);

  // After the expected reboot marker is consumed, retained AF plus the same
  // authoritative target still identifies the one-shot maintenance origin.
  classified = PowerManager::classify(
      status, ExpectedRebootMode::kNone, rtc::TimeState::kValid, target,
      protocol::ScheduleState::kPending, target);
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(protocol::BootReason::kRtcScheduledMaintenance),
      static_cast<uint8_t>(classified.reason));
  TEST_ASSERT_TRUE(classified.enterMaintenance);

  // AF without a matching authoritative one-shot target must not masquerade
  // as scheduled maintenance. TF remains the lower-priority valid source.
  classified = PowerManager::classify(
      status, ExpectedRebootMode::kNone, rtc::TimeState::kInvalidVl, 0U,
      protocol::ScheduleState::kPending, target);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::BootReason::kRtcTimer),
                          static_cast<uint8_t>(classified.reason));
  TEST_ASSERT_FALSE(classified.enterMaintenance);

  status.timerFlag = false;
  status.alarmFlag = false;
  classified = PowerManager::classify(
      status, ExpectedRebootMode::kNone, rtc::TimeState::kInvalidVl, 0U,
      protocol::ScheduleState::kNone, 0U);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::BootReason::kManualButton),
                          static_cast<uint8_t>(classified.reason));
  TEST_ASSERT_TRUE(classified.enterMaintenance);
}

void test_persistent_state_fresh_sequence_and_power_loss_safety() {
  FakeStateBackend backend;
  PersistentStateManager manager;
  bool fresh = false;
  TEST_ASSERT_TRUE(manager.begin(backend, 0x12345678U, fresh));
  TEST_ASSERT_TRUE(fresh);
  TEST_ASSERT_EQUAL_HEX32(0x12345678U, manager.state().persistentSessionId);
  uint32_t sequence = 0;
  TEST_ASSERT_TRUE(manager.allocateSequence(sequence));
  TEST_ASSERT_EQUAL_UINT32(1U, sequence);
  TEST_ASSERT_EQUAL_UINT32(2U, backend.state.nextSequence);

  PersistentStateManager rebooted;
  TEST_ASSERT_TRUE(rebooted.begin(backend, 0x99999999U, fresh));
  TEST_ASSERT_FALSE(fresh);
  TEST_ASSERT_EQUAL_HEX32(0x12345678U, rebooted.state().persistentSessionId);
  rebooted.state().filter.hasAccepted = true;
  rebooted.state().filter.lastAcceptedMm = 1234U;
  rebooted.state().filter.emaDistanceMm = 1233.5F;
  rebooted.state().command.commandId = 77U;
  rebooted.state().command.commandType =
      protocol::CommandType::kSetPollIntervalMinutes;
  rebooted.state().command.phase = CommandPhase::kCompleted;
  rebooted.state().command.result = protocol::CommandResultCode::kApplied;
  rebooted.state().scheduleState = protocol::ScheduleState::kPending;
  rebooted.state().scheduledMaintenanceUnix = 1'787'600'400U;
  TEST_ASSERT_TRUE(rebooted.commit());

  PersistentStateManager hardPowerCycle;
  TEST_ASSERT_TRUE(hardPowerCycle.begin(backend, 0x11111111U, fresh));
  TEST_ASSERT_FALSE(fresh);
  TEST_ASSERT_EQUAL_UINT32(1234U,
                           hardPowerCycle.state().filter.lastAcceptedMm);
  TEST_ASSERT_FLOAT_WITHIN(0.01F, 1233.5F,
                           hardPowerCycle.state().filter.emaDistanceMm);
  TEST_ASSERT_EQUAL_UINT32(77U,
                           hardPowerCycle.state().command.commandId);
  TEST_ASSERT_EQUAL_UINT32(1'787'600'400U,
                           hardPowerCycle.state().scheduledMaintenanceUnix);
  PowerEvent release{};
  release.bootReason = protocol::BootReason::kRtcTimer;
  release.shutdownReason = ShutdownReason::kPollSuccess;
  release.flagClearAttempted = true;
  TEST_ASSERT_TRUE(hardPowerCycle.appendPowerEvent(release));
  bool reconciled = false;
  TEST_ASSERT_TRUE(hardPowerCycle.reconcileLastFlagClearAfterColdBoot(
      false, reconciled));
  TEST_ASSERT_FALSE(reconciled);
  TEST_ASSERT_FALSE(hardPowerCycle.state().powerEvents[0].flagClearSucceeded);
  TEST_ASSERT_TRUE(hardPowerCycle.reconcileLastFlagClearAfterColdBoot(
      true, reconciled));
  TEST_ASSERT_TRUE(reconciled);
  TEST_ASSERT_TRUE(hardPowerCycle.state().powerEvents[0].flagClearSucceeded);
  PersistentStateManager reconciledReboot;
  TEST_ASSERT_TRUE(reconciledReboot.begin(backend, 0x22222222U, fresh));
  TEST_ASSERT_TRUE(reconciledReboot.state().powerEvents[0].flagClearSucceeded);
  TEST_ASSERT_TRUE(rebooted.allocateSequence(sequence));
  TEST_ASSERT_EQUAL_UINT32(2U, sequence);
  backend.failSaves = 1;
  TEST_ASSERT_FALSE(rebooted.allocateSequence(sequence));
  TEST_ASSERT_EQUAL_UINT32(0U, sequence);
  TEST_ASSERT_EQUAL_UINT32(3U, rebooted.state().nextSequence);

  backend.state.checksum ^= 1U;
  PersistentStateManager corruptRecovery;
  TEST_ASSERT_TRUE(corruptRecovery.begin(backend, 0xCAFEBABEU, fresh));
  TEST_ASSERT_TRUE(fresh);
  TEST_ASSERT_EQUAL_HEX32(0xCAFEBABEU,
                          corruptRecovery.state().persistentSessionId);
  TEST_ASSERT_EQUAL_UINT32(1U, corruptRecovery.state().nextSequence);
}

void test_history_fresh_append_wrap_order_and_corruption() {
  FakeHistoryBackend backend;
  NvsHistory history;
  TEST_ASSERT_TRUE(history.begin(backend));
  TEST_ASSERT_EQUAL_UINT16(0U, history.count());
  for (uint32_t sequence = 1; sequence <= build::kHistoryCapacity + 5U; ++sequence) {
    HistoryEntry entry{};
    entry.sequence = sequence;
    entry.rtcUnixTime = 1787600000U + sequence;
    entry.rawDistanceMm = 1000U + sequence;
    entry.filterState = static_cast<uint8_t>(FilterState::kStable);
    TEST_ASSERT_TRUE(history.append(entry));
  }
  TEST_ASSERT_EQUAL_UINT16(build::kHistoryCapacity, history.count());
  HistoryEntry output{};
  TEST_ASSERT_TRUE(history.at(0U, output));
  TEST_ASSERT_EQUAL_UINT32(6U, output.sequence);
  TEST_ASSERT_TRUE(history.at(build::kHistoryCapacity - 1U, output));
  TEST_ASSERT_EQUAL_UINT32(build::kHistoryCapacity + 5U, output.sequence);
  const uint16_t newestSlot = backend.metadata[
      backend.metadataPresent[1] && backend.metadata[1].generation >
          backend.metadata[0].generation ? 1 : 0].head == 0U
      ? build::kHistoryCapacity - 1U
      : backend.metadata[backend.metadataPresent[1] && backend.metadata[1].generation >
            backend.metadata[0].generation ? 1 : 0].head - 1U;
  backend.slots[newestSlot].checksum ^= 1U;
  TEST_ASSERT_FALSE(history.at(build::kHistoryCapacity - 1U, output));
  TEST_ASSERT_EQUAL_UINT32(1U, history.corruptEntries());
}

void test_history_metadata_power_loss_uses_previous_copy() {
  FakeHistoryBackend backend;
  NvsHistory history;
  TEST_ASSERT_TRUE(history.begin(backend));
  HistoryEntry one{};
  one.sequence = 1U;
  TEST_ASSERT_TRUE(history.append(one));
  backend.failMetadataWrite = true;
  HistoryEntry two{};
  two.sequence = 2U;
  TEST_ASSERT_FALSE(history.append(two));
  backend.failMetadataWrite = false;
  NvsHistory recovered;
  TEST_ASSERT_TRUE(recovered.begin(backend));
  TEST_ASSERT_EQUAL_UINT16(1U, recovered.count());
}

void test_history_web_windows_are_bounded_for_all_ring_states() {
  using namespace history_query;
  Page result = page(0U, 0U, 0U);
  TEST_ASSERT_EQUAL_UINT16(0U, result.returned);
  TEST_ASSERT_FALSE(result.hasNext);

  result = page(1U, 0U, 50U);
  TEST_ASSERT_EQUAL_UINT16(kMaximumPageSize, result.limit);
  TEST_ASSERT_EQUAL_UINT16(1U, result.returned);
  TEST_ASSERT_FALSE(result.hasNext);

  result = page(511U, 500U, 25U);
  TEST_ASSERT_EQUAL_UINT16(11U, result.returned);
  TEST_ASSERT_TRUE(result.hasPrevious);
  TEST_ASSERT_FALSE(result.hasNext);

  result = page(512U, 0U, UINT16_MAX);
  TEST_ASSERT_EQUAL_UINT16(kMaximumPageSize, result.returned);
  TEST_ASSERT_TRUE(result.hasNext);
  TEST_ASSERT_EQUAL_UINT16(kMaximumPageSize, result.nextOffset);
  result = page(512U, 500U, 25U);
  TEST_ASSERT_EQUAL_UINT16(12U, result.returned);
  TEST_ASSERT_FALSE(result.hasNext);

  TEST_ASSERT_EQUAL_UINT16(0U, chartPointCount(0U, 100U));
  TEST_ASSERT_EQUAL_UINT16(1U, chartPointCount(1U, 100U));
  const uint16_t points = chartPointCount(512U, UINT16_MAX);
  TEST_ASSERT_EQUAL_UINT16(kMaximumChartPoints, points);
  TEST_ASSERT_EQUAL_UINT16(0U, chartIndex(512U, points, 0U));
  TEST_ASSERT_EQUAL_UINT16(511U, chartIndex(512U, points, points - 1U));
  uint16_t previous = 0U;
  for (uint16_t i = 1U; i < points; ++i) {
    const uint16_t current = chartIndex(512U, points, i);
    TEST_ASSERT_GREATER_THAN_UINT16(previous, current);
    previous = current;
  }
}

void test_command_idempotency_and_storage_order() {
  FakeCommandEnvironment environment;
  CommandProcessor processor(environment);
  protocol::AckCommandPacket ack{};
  ack.commandId = 55U;
  ack.commandType = protocol::CommandType::kSetPollIntervalMinutes;
  ack.pollIntervalMinutes = 5U;
  const auto first = processor.handle(ack, "N1", 7U);
  TEST_ASSERT_TRUE(first.sendResult);
  TEST_ASSERT_FALSE(first.duplicate);
  TEST_ASSERT_EQUAL_INT(1, environment.receiptWrites);
  TEST_ASSERT_EQUAL_INT(1, environment.pollApplications);
  TEST_ASSERT_EQUAL_INT(1, environment.resultWrites);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::CommandResultCode::kApplied),
      static_cast<uint8_t>(first.packet.resultCode));
  const auto duplicate = processor.handle(ack, "N1", 7U);
  TEST_ASSERT_TRUE(duplicate.duplicate);
  TEST_ASSERT_EQUAL_INT(1, environment.pollApplications);
  TEST_ASSERT_EQUAL_INT(1, environment.receiptWrites);
  TEST_ASSERT_EQUAL_INT(1, environment.resultWrites);
}

void test_command_not_applied_when_receipt_persistence_fails() {
  FakeCommandEnvironment environment;
  environment.storageOkay = false;
  CommandProcessor processor(environment);
  protocol::AckCommandPacket ack{};
  ack.commandId = 1U;
  ack.commandType = protocol::CommandType::kEnterMaintenanceNow;
  const auto result = processor.handle(ack, "N1", 7U);
  TEST_ASSERT_EQUAL_INT(0, environment.enterApplications);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(protocol::CommandResultCode::kStorageError),
      static_cast<uint8_t>(result.packet.resultCode));
}

void test_filtering_and_config_migration_policy() {
  NodeConfig config = defaults();
  TEST_ASSERT_TRUE(static_cast<bool>(validateConfig(config)));
  TEST_ASSERT_EQUAL_UINT8(1U, pollMinutesFromLegacySeconds(1U));
  TEST_ASSERT_EQUAL_UINT8(1U, pollMinutesFromLegacySeconds(60U));
  TEST_ASSERT_EQUAL_UINT8(2U, pollMinutesFromLegacySeconds(61U));
  TEST_ASSERT_EQUAL_UINT8(255U, pollMinutesFromLegacySeconds(86400U));
  LegacyNodeConfigV1 legacy{};
  std::strcpy(legacy.nodeId, "GTH-FIELD-CAL");
  legacy.normalWakeIntervalSec = 61U;
  legacy.sonarBurstCount = 9U;
  legacy.referenceDistanceMm = 1725U;
  legacy.batteryCalibrationFactor = 1.125F;
  legacy.batteryCalibrationOffsetMv = -37;
  legacy.loraFrequencyMhz = 434.25F;
  legacy.ackTimeoutMs = 2400U;
  legacy.maintenanceTimeoutSec = 999U;
  const uint32_t legacyChecksum = legacyConfigV1Checksum(legacy);
  legacy.changingWakeIntervalSec = 13U;
  TEST_ASSERT_NOT_EQUAL(legacyChecksum, legacyConfigV1Checksum(legacy));
  const NodeConfig migrated = migrateLegacyConfigV1(legacy, "GTH-DEFAULT");
  TEST_ASSERT_EQUAL_STRING("GTH-FIELD-CAL", migrated.nodeId);
  TEST_ASSERT_EQUAL_UINT8(2U, migrated.pollIntervalMinutes);
  TEST_ASSERT_EQUAL_UINT8(9U, migrated.sonarBurstCount);
  TEST_ASSERT_EQUAL_UINT32(1725U, migrated.referenceDistanceMm);
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 1.125F,
                           migrated.batteryCalibrationFactor);
  TEST_ASSERT_EQUAL_INT16(-37, migrated.batteryCalibrationOffsetMv);
  TEST_ASSERT_FLOAT_WITHIN(0.0001F, 434.25F,
                           migrated.loraFrequencyMhz);
  TEST_ASSERT_EQUAL_UINT16(2400U, migrated.ackTimeoutMs);
  TEST_ASSERT_EQUAL_UINT16(300U, migrated.maintenanceTimeoutSec);
  TEST_ASSERT_TRUE(static_cast<bool>(validateConfig(migrated)));
  FilterMemory memory{};
  TemporalFilter filter(memory);
  for (uint8_t i = 0; i < 7U; ++i) (void)filter.evaluate(1500U, true, config);
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

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_protocol_v3_telemetry_golden_big_endian);
  RUN_TEST(test_protocol_v3_reference_sentinels_and_malformed_packets);
  RUN_TEST(test_ack_command_none_time_flags_and_all_commands);
  RUN_TEST(test_command_result_codec_all_result_codes);
  RUN_TEST(test_pcf_bcd_time_vl_and_boundaries);
  RUN_TEST(test_pcf_timer_level_mode_and_independent_flag_clear);
  RUN_TEST(test_pcf_alarm_encoding_and_schedule_horizon);
  RUN_TEST(test_power_transaction_preserves_active_alarm_until_final_release);
  RUN_TEST(test_boot_classification_precedence_and_alarm_target_match);
  RUN_TEST(test_persistent_state_fresh_sequence_and_power_loss_safety);
  RUN_TEST(test_history_fresh_append_wrap_order_and_corruption);
  RUN_TEST(test_history_metadata_power_loss_uses_previous_copy);
  RUN_TEST(test_history_web_windows_are_bounded_for_all_ring_states);
  RUN_TEST(test_command_idempotency_and_storage_order);
  RUN_TEST(test_command_not_applied_when_receipt_persistence_fails);
  RUN_TEST(test_filtering_and_config_migration_policy);
  return UNITY_END();
}
