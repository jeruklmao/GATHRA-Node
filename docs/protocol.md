# GATHRA LoRa Protocol 3

Firmware 2.1.1 uses Protocol 3 exclusively. All multi-byte integers are unsigned
big-endian unless marked signed; firmware encodes fields individually and never
transmits a C++ struct. SX1278 CRC is enabled, but Protocol 3 does not provide
HMAC, encryption, or node authentication.

## Common header

Let N be nodeIdLength and P = 5 + N.

| Offset | Size | Field | Value |
| ---: | ---: | --- | --- |
| 0 | 2 | magic | ASCII GT (47 54) |
| 2 | 1 | protocolVersion | 03 |
| 3 | 1 | messageType | 01 telemetry, 02 ACK_COMMAND, 03 COMMAND_RESULT |
| 4 | 1 | nodeIdLength | 1–24 |
| 5 | N | nodeId | ASCII A–Z, a–z, 0–9, hyphen, underscore |

Packets with a wrong version, malformed ID, unexpected length, invalid enum, reserved flag bit, or trailing byte are rejected.

## TELEMETRY (01)

The payload is 57 bytes, so total length is 62 + N. The maximum packet is 86 bytes for a 24-byte Node ID and fits the 96-byte radio buffer.

| Relative to P | Size | Field | Encoding |
| ---: | ---: | --- | --- |
| 0 | 4 | persistentSessionId | uint32 |
| 4 | 4 | sequence | uint32 |
| 8 | 4 | medianEchoUs | uint32 |
| 12 | 4 | rawDistanceMm | uint32 |
| 16 | 4 | acceptedDistanceMm | uint32 |
| 20 | 2 | madMm | uint16 |
| 22 | 2 | temperatureCentiC | int16 two's complement |
| 24 | 2 | humidityCentiPercent | uint16 |
| 26 | 2 | batteryMv | uint16 |
| 28 | 1 | validSamples | uint8 |
| 29 | 1 | totalSamples | uint8 |
| 30 | 1 | filterState | enum below |
| 31 | 2 | qualityFlags | bit mask |
| 33 | 2 | healthFlags | bit mask |
| 35 | 1 | bootReason | enum below |
| 36 | 1 | rtcState | enum below |
| 37 | 4 | rtcUnixTime | UTC seconds, or 0 when untrusted |
| 41 | 1 | pollIntervalMinutes | 1–255 |
| 42 | 1 | scheduleState | enum below |
| 43 | 4 | scheduledMaintenanceUnix | UTC seconds, or 0 for NONE |
| 47 | 4 | lastCommandId | 0 when command type is NONE |
| 51 | 1 | lastCommandType | command enum |
| 52 | 1 | lastCommandResult | result enum, FF for NONE |
| 53 | 4 | referenceDistanceMm | uint32 calibration reference; absolute offset 58 + N |

Sentinels: distance = FFFFFFFF, temperature = 8000, humidity = FFFF. `referenceDistanceMm=0` means calibration is not configured; every non-zero uint32 value is preserved exactly. Invalid RTC states require rtcUnixTime=0. Schedule state NONE requires scheduledMaintenanceUnix=0.

Filter states: 0 STABLE, 1 ACCEPTED, 2 VERIFY_RISE, 3 VERIFY_FALL, 4 TRANSIENT_REJECTED, 5 CHANGE_CONFIRMED, 6 UNCERTAIN, 7 INVALID.

Boot reasons: 0 RTC_TIMER, 1 RTC_SCHEDULED_MAINTENANCE, 2 MANUAL_BUTTON, 3 MAINTENANCE_REBOOT, 4 OTA_REBOOT, 5 UNKNOWN.

RTC states: 0 VALID, 1 INVALID_VL, 2 UNINITIALIZED, 3 I2C_ERROR. Schedule states: 0 NONE, 1 PENDING, 2 COMPLETED, 3 FAILED.

Quality bits: bit0 environment compensation, bit1 raw distance valid, bit2 accepted distance valid, bit3 installation limits applied, bit4 verification performed.

Health bits: bit0 sonar invalid, bit1 DHT invalid, bit2 environment stale, bit3 battery low, bit4 battery critical, bit5 radio error, bit6 unacknowledged, bit7 transient, bit8 uncertain, bit9 calibration missing, bit10 sensor degraded, bit11 battery ADC invalid.

## ACK_COMMAND (02)

The fixed ACK payload is 19 bytes followed by L command bytes; total length is 24 + N + L.

| Relative to P | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | persistentSessionId |
| 4 | 4 | telemetry sequence |
| 8 | 4 | gatewayUnixTime |
| 12 | 1 | flags; bit0=timeValid, all others zero |
| 13 | 4 | commandId |
| 17 | 1 | commandType |
| 18 | 1 | commandPayloadLength |
| 19 | L | command payload |

When timeValid is zero, gatewayUnixTime must be zero. Identity must match nodeId, persistentSessionId, and sequence exactly.

| Command | Code | L | Payload |
| --- | ---: | ---: | --- |
| NONE | 0 | 0 | commandId must be 0 |
| ENTER_MAINTENANCE_NOW | 1 | 0 | none |
| SCHEDULE_MAINTENANCE_AT | 2 | 4 | UTC Unix seconds |
| SET_POLL_INTERVAL_MINUTES | 3 | 1 | 1–255 |

Gateway UTC is generated immediately before ACK construction. A command stays pending until a matching COMMAND_RESULT arrives.

## COMMAND_RESULT (03)

The payload is 15 bytes; total length is 20 + N.

| Relative to P | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | persistentSessionId |
| 4 | 4 | commandId |
| 8 | 1 | commandType |
| 9 | 1 | resultCode |
| 10 | 1 | effectivePollIntervalMinutes |
| 11 | 4 | effective scheduled-maintenance UTC |

Result codes: 0 APPLIED, 1 ALREADY_APPLIED, 2 INVALID_ARGUMENT, 3 RTC_UNAVAILABLE, 4 RTC_TIME_UNTRUSTED, 5 SCHEDULE_UNREPRESENTABLE, 6 STORAGE_ERROR, 7 INTERNAL_ERROR.

The Gateway sends no ACK for this packet. It repeats an unconfirmed command in later telemetry ACKs. The Node persists receipt before applying, persists the result before reporting it, and re-sends the stored result for a duplicate command ID without repeating the side effect.

## Session and sequence safety

persistentSessionId is generated once and survives timer power cycles, manual cycles, resets, and OTA. Only an explicit identity/protocol-state reset may replace it. The Node reads nextSequence, increments and persists it before transmission. Power loss can create a gap but cannot reuse an allocated sequence. Gateway deduplication key is nodeId + persistentSessionId + sequence.

## Scheduled alarm constraints

The PCF8563 alarm has minute, hour, day-of-month, and weekday comparators—no month or year. GATHRA enables minute/hour/day and disables weekday. The NVS UTC target remains authoritative. Targets must be minute-aligned, at least 60 seconds ahead, and no more than 27 days ahead; otherwise the command returns SCHEDULE_UNREPRESENTABLE. On AF wake the Node also requires valid RTC UTC and a target match within the bounded five-minute window.
