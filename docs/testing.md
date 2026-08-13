# Validation record

This record separates observed hardware evidence from implementation and host tests. It does not claim a gateway or physical-button actuation that was unavailable.

## Toolchain and board inventory

- PlatformIO Core 6.1.19; `espressif32@7.0.1`
- Arduino-ESP32 package `3.20017.241212` (upstream 2.0.17), ESP-IDF 4.4.x
- RadioLib 7.7.1; ArduinoJson 7.4.2; DHT 1.4.7; Unified Sensor 1.1.15
- ESP32-C3 rev 0.4, USB Serial/JTAG, MAC `10:00:3B:D4:BC:FC`
- embedded XMC flash detected by esptool: 4 MB
- development serial device: `/dev/ttyACM0` (rediscover with `pio device list`)
- AP HIL used Realtek RTL8188EUS `wlp0s20f0u1`; Intel PCI `wlp0s20f3` remained on the internet WLAN and was not used for node testing

## Automated logic and build

`tools/run-native-tests.sh` passes 11/11 Unity tests. Coverage includes deterministic big-endian telemetry, invalid protocol framing, ACK codec/matching/rejection, bounded retries, median/MAD, transient rejection, persistent rise/fall confirmation, zero-MAD behavior, invalid measurements, configuration range validation, and retained-ring empty/fill/wrap/schema reset.

The final clean production build passes with 885,144 bytes used from a 1,900,544-byte OTA slot (46.6%) and 50,644 bytes static RAM from 327,680 bytes (15.5%). The custom table fits the detected 4 MB flash and contains two equal OTA slots and a 64 KiB core-dump partition.

## HIL results

| Area | Status | Observed evidence |
|---|---|---|
| board discovery and USB flash | PASS | esptool identified ESP32-C3 rev 0.4, MAC above, 4 MB XMC flash; image/hash upload completed |
| production boot and serial | PASS | firmware logged `1.0.0 build=production`, reset/wake reasons, NVS/RTC/OTA state at 115200 baud |
| battery ADC | PASS | battery-only power: five production reads were raw ADC 1760–1768, calibrated divider 1302–1305 mV, reconstructed battery 3906–3915 mV (mean 3910 mV), all `adcValid=true`; low/critical bits clear |
| DHT22 | PASS | repeated compensated readings, e.g. 32.9 °C and 67.8% RH |
| HY-SRF05 ISR/burst/MAD | PASS | repeated 7/7 bursts; each logged one rising/one falling edge; examples 1718 mm/MAD 4 and 1691 mm/MAD 0 |
| filter/history diagnostics | PASS | raw and accepted values exposed independently; calibration-unconfigured height was `null`; post-sleep 1721 mm classified `STABLE` against retained 1722 mm |
| SX1278 initialization/TX | PASS | initialized at 433 MHz, 125 kHz, SF10, CR4/6, 17 dBm, sync `0x12`; transmitted 56-byte v1 payloads |
| bounded missing ACK | PASS | each manual/autonomous test logged exactly three attempts with 1800 ms receive windows and randomized backoff, then slept/returned without hanging |
| deep sleep and RTC retention | PASS | timer and GPIO wake setup returned `ESP_OK`; timer wakes observed; session/sequence/history survived deep sleep and sequence advanced 1→2 |
| maintenance SoftAP | PASS | WPA2 SSID `GATHRA-NODE-GTH-10003BD4BCFC`, BSSID `10:00:3B:D4:BC:FD`, 192.168.4.1; Realtek client received 192.168.4.2 |
| offline dashboard/API | PASS | root returned HTTP 200 with local HTML/CSS/JS/canvas and no CDN; status/config/history/log APIs, Measure Now, calibration, radio test, reboot, and deferred Exit response exercised |
| config/NVS | PASS | SF13 returned HTTP 422; 301-second candidate saved, survived reboot, and was restored to required 300 seconds; numeric parsing now rejects overflow before narrowing |
| calibration | PASS | stable 1718 mm capture produced derived 0 mm; clearing restored unconfigured reference and `null` height |
| inactivity policy | PASS | with temporary 60-second test value, passive status polls returned 200 through 55 s but AP shut down at 60 s; NVS was restored and read back as 300 s |
| physical button wake/long press | NOT TESTED | no physical actuator was available; production GPIO2 debounce, ISR latch, active-low wake, long-press, and release-before-sleep paths are implemented |
| valid OTA upload and boot validation | PASS | multipart HTTP uploaded 930,544-byte production image; `ota_1` booted `PENDING_VERIFY`, application checks marked it `VALID`, and durable one-shot returned to maintenance |
| bootloader rollback | PASS | rollback-test booted on `ota_0` as `PENDING_VERIFY`, intentionally restarted before validation, then bootloader returned to `production` on valid `ota_1` |
| gateway ACK end-to-end | BLOCKED | no gateway hardware exists; codec/matching are host-tested and node RX timeout is HIL-tested only |

## Battery-only follow-up

The earlier USB-powered validation produced raw ADC 1–2 and 0–1 mV because the battery was intentionally disconnected to prevent a USB/battery rail conflict. After switching to battery-only power, the immutable GPIO0 path produced a stable, plausible result across five independent production acquisitions:

| Sample | Raw ADC | Divider mV | Battery mV | Valid |
|---:|---:|---:|---:|:---:|
| 1 | 1767 | 1302 | 3906 | yes |
| 2 | 1768 | 1305 | 3915 | yes |
| 3 | 1767 | 1304 | 3912 | yes |
| 4 | 1767 | 1304 | 3912 | yes |
| 5 | 1760 | 1302 | 3906 | yes |

Configuration readback confirmed nominal calibration factor 1.0, offset 0 mV, low threshold 3500 mV, and critical threshold 3300 mV. No battery health bit was set. The previous hardware-fault hypothesis is withdrawn.

## Commands exercised

```bash
pio device list
pio run -e esp32-c3-devkitm-1 --target clean
pio run -e esp32-c3-devkitm-1
tools/run-native-tests.sh
pio run -e hil --target upload --upload-port /dev/ttyACM0
pio run -e rollback-test
/home/fadhli/.platformio/penv/bin/python tools/serial_capture.py --port /dev/ttyACM0 --duration 75
nmcli connection up GATHRA-NODE-GTH-10003BD4BCFC ifname wlp0s20f0u1
curl --interface wlp0s20f0u1 http://192.168.4.1/api/status
curl --interface wlp0s20f0u1 -X POST http://192.168.4.1/api/measure
curl --interface wlp0s20f0u1 -F firmware=@firmware.bin http://192.168.4.1/api/ota
```

The final board image is the production profile, not HIL or rollback-test. Temporary test configuration was restored to the documented defaults.
