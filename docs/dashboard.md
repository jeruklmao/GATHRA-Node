# Node maintenance dashboard

Manual, scheduled, maintenance-command, OTA-reboot, and maintenance-reboot paths open the existing SoftAP:

- SSID: GATHRA-NODE-<node suffix>
- WPA2 password: sman35jakarta
- address: http://192.168.4.1/
- fixed maximum lifetime: 300 seconds

Passive status/history/log polling never extends the deadline.

## Status

The dashboard exposes firmware 2.1.1 and Protocol 3, logical boot reason, maintenance source/deadline, PCF communication and Control_status values, TF/AF/TIE/AIE/level state, RTC UTC and VL validity, poll interval and next expected poll, one-shot schedule state/UTC, persistent session and next sequence, last Gateway UTC sync, last durable command/result, persistent history utilization, power-off readiness, radio result, sensor/calibration state, OTA partition/state, and the bounded power-event diagnostics. A battery-powered flag-clear result is reconciled to success on the next genuine `POWER_ON` boot, since the rail normally disappears before a post-clear NVS write can complete.

Obsolete button state, ESP deep-sleep wake reason, and seconds-based sleep controls are absent.

## Actions

Measure Now runs the production sensor/filter path and persists one NVS history entry. History reads the chronological 512-slot NVS ring and remains available after complete power removal. Configuration retains all filtering, calibration, battery, LoRa and ACK controls; pollIntervalMinutes is bounded to 1–255 and maintenanceTimeoutSec to 60–300.

Send Test Packet allocates/persists a real sequence and performs a real Protocol 3 exchange. Any command in its ACK is processed with the same durable/idempotent semantics as a normal poll.

Finish Maintenance schedules a verified normal wake and begins the centralized release transaction. Reboot stores MAINTENANCE_REBOOT first. OTA stores OTA_REBOOT and verifies the current RTC latch before restarting.

## Bounded history API

`GET /api/history?offset=0&limit=12` returns chronological full records. The default and hard maximum page size are both 12; larger values are clamped. The response includes `count`, `capacity`, `offset`, `limit`, `returned`, `hasPrevious`, `hasNext`, and nullable `nextOffset`. The dashboard provides Previous/Next controls and never polls this route in the background.

`GET /api/history/chart?series=rawDistanceMm&maxPoints=100` reads at most 100 uniformly distributed chronological records while retaining the oldest and newest endpoints. Supported series are `rawDistanceMm`, `acceptedDistanceMm`, `waterHeightMm`, `temperatureC`, `humidityPercent`, and `batteryMv`. Compact chart entries use `s` (sequence), `v` (nullable value), and `f` (numeric filter state). The maximum is 100 points.

Both APIs have bounded NVS reads and bounded JSON size. The dashboard HTML and history responses are sent in 512-byte nonblocking socket writes. Each request has a 2.5-second deadline; EAGAIN/backpressure is retried only within that deadline, while disconnects or other socket errors close the request. Expensive work yields between writes, but the task watchdog is not disabled or extended. A failed admin response is deliberately lower priority than keeping the Node alive.

## Firmware 2.1.1 watchdog fix

Firmware 2.1.0 loaded all 512 NVS records into one ArduinoJson document and one `String`, then passed the complete body to `WiFiClient::write`. Arduino-ESP32 2.x retries an unwritable socket ten times with a one-second `select` timeout. On the full field history, `/api/history` advertised 10,106 bytes, delivered no body to the client, and remained inside the synchronous write until `loopTask` triggered TASK_WDT. Feeding or lengthening the watchdog would only hide this unbounded network path, so 2.1.1 bounds the data, memory, NVS work, and socket wait instead.

## Buzzer

GPIO2 is active HIGH. Patterns are: poll start one 100 ms beep; manual latch two 100 ms beeps; maintenance one 100 ms beep every three seconds without blocking WebServer; acknowledged success two 100 ms beeps; error/no ACK three 100 ms beeps; maintenance shutdown one 300 ms beep. GPIO is forced LOW before final release.
