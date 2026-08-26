# GATHRA Node

GATHRA Node firmware 2.1.0 implements LoRa Protocol 3 and true PCF8563-controlled hard-power cycling on an ESP32-C3 Super Mini. The ESP32 does not deep-sleep during normal production operation: it verifies the next RTC wake, clears the current level interrupt only at the final shutdown step, and the AO3401A removes power from the switched electronics.

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

The following survive complete power removal: Node ID/configuration and calibration, persistent session and next sequence, filter/EMA/candidate state, history, poll interval, one-shot maintenance target, RTC sync metadata, command receipt/result, reboot marker, maintenance deadline, and eight bounded power diagnostics.

## Safety limitations

If the ESP cannot execute at all while PCF INT is latched, software cannot clear the flag; the current hardware has no independent latch timeout/watchdog. Protocol 3 uses radio CRC but no HMAC/authentication. Gateway command control is local-dashboard only and Backend remote command scheduling is intentionally absent.
