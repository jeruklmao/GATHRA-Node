# GATHRA Node

GATHRA Node firmware 2.1.1 implements LoRa Protocol 3 and true PCF8563-controlled hard-power cycling on an ESP32-C3 Super Mini. The ESP32 does not deep-sleep during normal production operation: it verifies the next RTC wake, clears the current level interrupt only at the final shutdown step, and the AO3401A removes power from the switched electronics.

The authoritative wiring is docs/hardware/GATHRA_netlist.xml. Current GPIO assignments are centralized in include/board_pins.hpp:

| GPIO | Function |
| ---: | --- |
| 0 | battery ADC |
| 1, 3 | SX1278 RST, DIO0 |
| 4, 5, 6, 7 | SX1278 SCK, MISO, MOSI, NSS |
| 8, 9 | PCF8563 SDA, SCL with ESP internal pull-ups |
| 10 | DHT22 data |
| 20, 21 | HY-SRF05 trigger, echo |
| 2 | active-HIGH buzzer |

There is no ESP button input. The physical button only pulls the P-channel MOSFET gate low. On a manual boot, firmware creates a fast PCF8563 TF level latch, verifies it, beeps twice, and opens the maintenance AP. The AP password remains sman35jakarta and its maximum lifetime is 300 seconds.

## Runtime

A normal RTC timer boot performs battery, DHT22, seven-ping sonar acquisition, median/MAD and persistent temporal filtering, allocates and persists its sequence before RF transmission, sends v3 telemetry containing the persisted calibration reference, accepts only a matching v3 ACK, applies trusted Gateway UTC and an optional durable command, persists history/state, rearms the one-minute RTC timer, and releases external power.

Supported commands are ENTER_MAINTENANCE_NOW, SCHEDULE_MAINTENANCE_AT, and SET_POLL_INTERVAL_MINUTES. Commands are persisted before side effects and duplicate IDs re-send the stored result.

See docs/power-management.md, docs/pcf8563.md, docs/protocol.md, and docs/architecture.md.

## Build and test

~~~bash
pio test -e native
pio run -e esp32-c3-devkitm-1
pio run -e hil
~~~

Use the exact USB serial by-id path after positively mapping the board; never assume ttyACM numbering. The hil profile forces the dashboard on while retaining the real boot classification. USB can validate register/state logic but cannot prove battery-rail power removal.

## Persistent storage

Flash is 4 MiB with two 1.5 MiB OTA slots. A legacy 20 KiB NVS partition is retained read-only as a Protocol/config v1 migration source; all current configuration/state/history uses the 896 KiB `nvs_v2` partition. History uses 512 fixed 38-byte circular slots plus dual generation metadata: 8 h 32 min at one-minute polling or 3 d 13 h 20 min at the ten-minute default.

The maintenance dashboard never materializes or transmits the whole history ring. Full records use a 12-entry paginated API, charts use at most 100 uniformly downsampled points, and history is loaded only on entry or explicit refresh. Dashboard writes are divided into bounded socket chunks with a 2.5-second request deadline so a slow or disconnected client cannot starve `loopTask`.

The following survive complete power removal: Node ID/configuration and calibration, persistent session and next sequence, filter/EMA/candidate state, history, poll interval, one-shot maintenance target, RTC sync metadata, command receipt/result, reboot marker, maintenance deadline, and eight bounded power diagnostics.

## Safety limitations

Normal boots follow one orchestrated lifecycle:

```text
BOOT → ACQUIRE → FILTER → [VERIFY] → TRANSMIT → SLEEP
  └──────────────── button/GPIO wake ───────────→ MAINTENANCE
```

Acquisition is deliberately sequential: battery, DHT/environment snapshot, seven-ping sonar burst, filtering/verification, then LoRa TX/ACK. DHT failure falls back to reference acoustic conversion and sets health flags rather than disabling sonar. Suspicious changes are retained as raw data and verified before they can affect the EMA.

The default normal interval is 60 seconds. Uncertain or changing measurements schedule a 12-second recheck. Timer and active-low GPIO2 wake sources are armed before deep sleep.

## Maintenance mode

Pressing the button wakes the sleeping node into maintenance or requests maintenance at the next safe boundary while awake. In maintenance, a two-second button hold exits to sleep.

The node starts a WPA2 SoftAP:

- SSID: `GATHRA-NODE-<node-id suffix>`
- Password: `sman35jakarta`
- Dashboard: `http://192.168.4.1/`

The password is intentionally hardcoded and not editable in v1. This is a known v1 security limitation, not a secret. The dashboard has no additional login and should only be exposed through the node-local AP.

The dashboard is self-contained: no CDN, DNS, or internet access is required. It provides status, real production-pipeline measurements, RTC history graphing, full runtime configuration, calibration, LoRa tests, bounded logs, OTA, reboot, and exit controls. Passive status/history/log polling does not refresh the default five-minute inactivity timeout; actual interaction does.

## Configuration and identity

`NodeConfig` is validated before application and stored in NVS with schema, size, and checksum checks. Invalid or incompatible storage falls back to safe documented defaults. Radio changes use apply/test/save with rollback to the previous settings on initialization failure.

The default logical ID is deterministic: `GTH-` followed by the 12 uppercase hexadecimal Wi-Fi MAC digits, for example `GTH-10003BD4BCFC`. A customized logical ID is stored in NVS. The immutable MAC remains separately visible.

No mounting reference is invented. Raw distance works without calibration, while derived water height remains unavailable until a reference is explicitly entered or captured from a stable accepted measurement.

## OTA

Upload PlatformIO's `.pio/build/esp32-c3-devkitm-1/firmware.bin` from the dashboard. The inactive slot is written through the Arduino `Update` API. Native ESP-IDF rollback support is enabled in the installed SDK. Arduino's early auto-validation is overridden; a pending image is marked valid only after configuration, retained-state, and partition-layout boot checks pass. The OTA reboot sets a durable one-shot NVS marker (with RTC fallback) to return to maintenance, then consumes it after the validated reboot.

See `docs/ota.md` for the controlled rollback-test profile and recovery procedure.

## Initial field-tuning defaults

These are conservative engineering starting points, not scientifically finalized site thresholds:

- sonar: 7 pings, minimum 5 valid, 60 ms spacing
- Hampel: 7 accepted samples, K = 3, 50 mm absolute floor
- sudden change: 100 mm
- apparent rise: 5 observations, 4 confirmations, 2 s, 75 mm tolerance
- apparent fall: 5 observations, 4 confirmations, 5 s, 100 mm tolerance
- EMA alpha: 0.25, applied only after verification
- normal/changing wake: 60 s / 12 s
- battery low/critical: 3500 / 3300 mV (engineering defaults; chemistry-specific control is intentionally absent)
- LoRa: 433.0 MHz, 125 kHz, SF10, CR 4/6, 17 dBm, sync `0x12`
- ACK: 1800 ms, two retries after the initial attempt (three total)
- maintenance timeout: 300 s

All are dashboard-editable except physical pins and the AP password.

## v1 boundaries

v1 does not provide LoRa HMAC/node authentication, gateway hardware, backend ingestion, or end-to-end gateway ACK validation. Radio CRC detects transmission errors but is not authentication. Measurement/filter history is intentionally RTC-retained only: it survives deep sleep, not power loss, hard reset, or reflashing.

Battery-only validation passes on the required GPIO0 divider input. Five consecutive production measurements reported raw ADC 1760–1768, divider voltage 1302–1305 mV, and reconstructed battery voltage 3906–3915 mV, with the `BATTERY_ADC_INVALID`, `BATTERY_LOW`, and `BATTERY_CRITICAL` flags all clear. Earlier near-zero USB-powered readings occurred while the battery was intentionally disconnected to prevent a power-rail conflict; they were not evidence of a wiring fault.

Further details: [architecture](docs/architecture.md), [hardware](docs/hardware/README.md), [protocol](docs/protocol.md), [dashboard](docs/dashboard.md), [calibration](docs/calibration.md), [testing](docs/testing.md), and [OTA](docs/ota.md).

---

Copyright © 2026 GATHRA Project. All rights reserved.

Source code and documentation in this repository are publicly viewable for inspection, academic review, and evaluation. No permission is granted to reproduce, redistribute, modify, commercialize, or create derivative works except where explicitly permitted by the repository's license or by written permission from the copyright holder.

If you use GATHRA in academic or research work, please provide appropriate attribution to the GATHRA Project and its associated publications.
