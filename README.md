# GATHRA Node

GATHRA Node firmware **2.1.1** measures flood-water distance and sends telemetry
to one GATHRA Gateway over **LoRa Protocol 3**. It runs on an ESP32-C3 Super
Mini with an SX1278, HY-SRF05 ultrasonic sensor, DHT22, PCF8563 RTC, battery
monitor, and active-HIGH buzzer.

The PCF8563 and AO3401A control the production hard-power lifecycle. Normal
polling does not use ESP deep sleep: firmware verifies the next RTC wake,
clears the active RTC interrupt only during the final shutdown transaction,
and then external power is removed from the switched electronics.

## Hardware

`include/board_pins.hpp` is the wiring authority.

| GPIO | Function |
| ---: | --- |
| 0 | battery ADC |
| 1, 3 | SX1278 RST, DIO0 |
| 4, 5, 6, 7 | SX1278 SCK, MISO, MOSI, NSS |
| 8, 9 | PCF8563 SDA, SCL |
| 10 | DHT22 data |
| 20, 21 | HY-SRF05 trigger, echo |
| 2 | active-HIGH buzzer |

The physical maintenance button pulls the power-latch gate low; it is not an
ESP32 input. A manual power-up establishes and verifies an RTC timer latch,
then opens the node-local maintenance access point. See
[hardware](docs/hardware/README.md) and [power management](docs/power-management.md).

## Measurement and telemetry

A normal timer wake performs this bounded sequence:

```text
battery -> DHT/environment -> ultrasonic burst -> filter/verify
        -> persist sequence -> Protocol 3 telemetry/ACK
        -> persist history/state -> program next RTC wake -> power off
```

The default ultrasonic burst uses seven pings with at least five valid samples.
Firmware calculates median distance and MAD, applies temperature/humidity sound
speed compensation when available, then uses Hampel filtering, rise/fall
confirmation, and EMA smoothing. `rawDistanceMm` is the burst median;
`acceptedDistanceMm` is the stable filtered value used for water-height
derivation.

`referenceDistanceMm` is an operator-configured installation datum. A value of
zero means it is unset. When both reference and accepted distance are available:

```text
waterHeightMm = referenceDistanceMm - acceptedDistanceMm
```

Protocol 3 telemetry includes raw and accepted distance, environmental and
battery measurements, filter/quality/health diagnostics, RTC and schedule
state, command result state, poll interval, and the Node reference distance.
The exact wire layout is documented in [protocol.md](docs/protocol.md).

The default poll interval is **10 minutes** and the configured range is 1–255
minutes. A measurement that requires a quicker follow-up is scheduled at the
one-minute minimum supported by the RTC timer.

## Commands and persistence

The Gateway can include one command in a matching ACK:

- `ENTER_MAINTENANCE_NOW`
- `SCHEDULE_MAINTENANCE_AT`
- `SET_POLL_INTERVAL_MINUTES`

Command receipt and result are persisted before reporting success. Repeated
command IDs return the stored result without repeating the side effect.

The `nvs_v2` partition stores validated configuration, persistent session and
next sequence, filter state, 512 measurement-history records, maintenance and
RTC state, command state, OTA markers, and bounded power diagnostics. These
records survive complete external power removal.

## Maintenance dashboard

Maintenance mode provides a self-contained node-local dashboard for status,
measurement, calibration, configuration, LoRa diagnostics, bounded logs,
history, OTA, reboot, and controlled power-off. Its configured lifetime is
60–300 seconds and defaults to 300 seconds; passive polling does not extend it.

History is bounded at every interface:

- `GET /api/history?offset=0&limit=12` returns chronological pages; default and
  maximum page size are 12.
- `GET /api/history/chart?series=rawDistanceMm&maxPoints=100` returns at most
  100 uniformly sampled points while retaining the endpoints.
- the dashboard loads history on entry or explicit refresh and does not poll
  full records in the background.

See [dashboard.md](docs/dashboard.md) for the current endpoint contract.

## OTA

Browser OTA writes PlatformIO's
`.pio/build/esp32-c3-devkitm-1/firmware.bin` to the inactive application slot.
A pending image is marked valid only after configuration, NVS, partition,
RTC, history, sensor, and radio initialization checks pass. OTA and maintenance
reboots preserve the active RTC latch and remaining maintenance deadline. See
[ota.md](docs/ota.md).

## Build and test

```bash
pio test -e native
pio run -e esp32-c3-devkitm-1
pio run -e hil
```

The `hil` profile forces maintenance mode for hardware checks. Hardware flashing
and battery-rail validation require a positively identified board and are not
performed by the automated commands. See [testing.md](docs/testing.md).

## Safety and security

Protocol 3 uses radio CRC but does not provide encryption or Node
authentication. Pairing is an operational allow-list. The maintenance dashboard
has no application-level login and must remain limited to the node-local WPA2
network.

GATHRA measurements and derived flood levels are modeled observations, not a
public-safety guarantee. Missing, uncertain, or stale information must never be
treated as evidence that an area is safe.

---

Copyright © 2026 GATHRA Project. All rights reserved.

Source code and documentation in this repository are publicly viewable for inspection, academic review, and evaluation. No permission is granted to reproduce, redistribute, modify, commercialize, or create derivative works except where explicitly permitted by the repository's license or by written permission from the copyright holder.

If you use GATHRA in academic or research work, please provide appropriate attribution to the GATHRA Project and its associated publications.
