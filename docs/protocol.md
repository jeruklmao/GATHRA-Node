# GATHRA binary LoRa protocol v1

Multi-byte integers use network byte order (big-endian). Every field is encoded individually; firmware never transmits a raw C++ struct. SX1278 packet CRC is enabled. Protocol v1 intentionally has no HMAC or node authentication.

Common prefix:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | magic `0x47 0x54` (`GT`) |
| 2 | 1 | protocol version `0x01` |
| 3 | 1 | message type (`0x01` telemetry, `0x02` ACK) |
| 4 | 1 | logical node-ID length `N` (1–24) |
| 5 | N | node ID ASCII bytes, without NUL |

## Telemetry (`0x01`)

The following offsets are relative to `5 + N`:

| Relative | Size | Field | Encoding |
|---:|---:|---|---|
| 0 | 4 | boot/session ID | unsigned |
| 4 | 4 | sequence | unsigned |
| 8 | 4 | median echo duration | microseconds |
| 12 | 4 | raw median distance | millimetres; `0xFFFFFFFF` unavailable |
| 16 | 4 | accepted/EMA distance | millimetres; `0xFFFFFFFF` unavailable |
| 20 | 2 | burst MAD | millimetres |
| 22 | 2 | temperature | signed centi-°C; `0x8000` unavailable |
| 24 | 2 | humidity | unsigned centi-%RH; `0xFFFF` unavailable |
| 26 | 2 | battery | millivolts |
| 28 | 1 | valid sonar samples | count |
| 29 | 1 | total sonar samples | count |
| 30 | 1 | filter state | table below |
| 31 | 2 | quality flags | bit set |
| 33 | 2 | health flags | bit set |

Payload length is `40 + N` bytes (64 bytes at the maximum ID length). The default 16-character ID produces 56 bytes. Derived water height is not sent because it is installation configuration; gateways can derive it when a trusted reference exists.

Filter states:

| Value | State |
|---:|---|
| 0 | STABLE |
| 1 | ACCEPTED |
| 2 | VERIFY_RISE |
| 3 | VERIFY_FALL |
| 4 | TRANSIENT_REJECTED |
| 5 | CHANGE_CONFIRMED |
| 6 | UNCERTAIN |
| 7 | INVALID |

Quality flags:

| Bit | Hex | Meaning |
|---:|---:|---|
| 0 | `0x0001` | environment compensation used |
| 1 | `0x0002` | raw distance valid |
| 2 | `0x0004` | accepted distance valid |
| 3 | `0x0008` | explicit installation limits applied |
| 4 | `0x0010` | verification performed |

Health flags:

| Bit | Hex | Meaning |
|---:|---:|---|
| 0 | `0x0001` | SONAR_INVALID |
| 1 | `0x0002` | DHT_INVALID |
| 2 | `0x0004` | ENV_STALE |
| 3 | `0x0008` | BATTERY_LOW |
| 4 | `0x0010` | BATTERY_CRITICAL |
| 5 | `0x0020` | RADIO_ERROR |
| 6 | `0x0040` | TX_UNACKED |
| 7 | `0x0080` | FILTER_TRANSIENT |
| 8 | `0x0100` | FILTER_UNCERTAIN |
| 9 | `0x0200` | CALIBRATION_MISSING |
| 10 | `0x0400` | SENSOR_DEGRADED |
| 11 | `0x0800` | BATTERY_ADC_INVALID |

`TX_UNACKED` is necessarily known only after the current packet's ACK window closes. It is retained in local diagnostics/history; a packet that never reached a gateway cannot report its own future ACK failure.

## ACK (`0x02`)

After the common prefix:

| Relative | Size | Field |
|---:|---:|---|
| 0 | 4 | matching boot/session ID |
| 4 | 4 | matching sequence |

Length is `13 + N` bytes. A node accepts an ACK only if logical node ID, boot/session ID, and sequence all match. Wrong version/type, malformed length, trailing bytes, or any identity mismatch is rejected.

TX flow is initial attempt plus at most two configurable retries, each followed by a bounded 1800 ms default receive window. The timeout covers the selected SF10 telemetry airtime plus a gateway turnaround margin. Retry backoff is randomly 100–350 ms. Missing ACK never prevents sleep.
