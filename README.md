# GATHRA Node Firmware

Production firmware v1.0.0 for the battery/solar-powered GATHRA flood-monitoring sensor node. The node measures sensor-to-surface distance, preserves measurement quality and health context, sends compact binary LoRa telemetry, and deep-sleeps between acquisitions. Flood-routing risk classification remains a backend responsibility.

## Hardware

The target is an ESP32-C3 with a DHT22, HY-SRF05, SX1278 (433 MHz), active-low maintenance button, and a 10 kΩ/5 kΩ battery divider. Immutable production GPIO assignments are centralized in `include/board_pins.hpp` and match the Fritzing/netlist sources in `docs/hardware/`.

The 5 V sensor rail is not switched by the ESP32. DHT22 and HY-SRF05 therefore remain powered during ESP deep sleep; firmware cannot eliminate that hardware power cost.

## Build, flash, and monitor

Requirements: PlatformIO Core 6.x, a USB-connected ESP32-C3, and the pinned dependencies in `platformio.ini`.

```bash
pio run -e esp32-c3-devkitm-1
pio device list
pio run -e esp32-c3-devkitm-1 -t upload --upload-port /dev/ttyACM0
pio device monitor --port /dev/ttyACM0 --baud 115200
```

The production image uses Arduino-ESP32 2.0.17/ESP-IDF 4.4 through pinned PlatformIO `espressif32@7.0.1`. USB CDC is enabled as required. The custom 4 MB partition table has two 1.8125 MiB OTA application slots plus a bounded 64 KiB flash core-dump partition.

Run pure-logic tests with:

```bash
tools/run-native-tests.sh
```

The wrapper uses system GCC when available. On a no-root host without GCC it can use the pinned documented Zig fallback after:

```bash
~/.platformio/penv/bin/pip install ziglang==0.15.2
```

## Lifecycle

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
