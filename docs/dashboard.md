# Node maintenance dashboard

Manual, scheduled, maintenance-command, OTA-reboot, and maintenance-reboot paths open the existing SoftAP:

- SSID: GATHRA-NODE-<node suffix>
- WPA2 password: sman35jakarta
- address: http://192.168.4.1/
- fixed maximum lifetime: 300 seconds

Passive status/history/log polling never extends the deadline.

## Status

The dashboard exposes firmware 2.1.0 and Protocol 3, logical boot reason, maintenance source/deadline, PCF communication and Control_status values, TF/AF/TIE/AIE/level state, RTC UTC and VL validity, poll interval and next expected poll, one-shot schedule state/UTC, persistent session and next sequence, last Gateway UTC sync, last durable command/result, persistent history utilization, power-off readiness, radio result, sensor/calibration state, OTA partition/state, and the bounded power-event diagnostics. A battery-powered flag-clear result is reconciled to success on the next genuine `POWER_ON` boot, since the rail normally disappears before a post-clear NVS write can complete.

Obsolete button state, ESP deep-sleep wake reason, and seconds-based sleep controls are absent.

## Actions

Measure Now runs the production sensor/filter path and persists one NVS history entry. History reads the chronological 512-slot NVS ring and remains available after complete power removal. Configuration retains all filtering, calibration, battery, LoRa and ACK controls; pollIntervalMinutes is bounded to 1–255 and maintenanceTimeoutSec to 60–300.

Send Test Packet allocates/persists a real sequence and performs a real Protocol 3 exchange. Any command in its ACK is processed with the same durable/idempotent semantics as a normal poll.

Finish Maintenance schedules a verified normal wake and begins the centralized release transaction. Reboot stores MAINTENANCE_REBOOT first. OTA stores OTA_REBOOT and verifies the current RTC latch before restarting.

## Buzzer

GPIO2 is active HIGH. Patterns are: poll start one 100 ms beep; manual latch two 100 ms beeps; maintenance one 100 ms beep every three seconds without blocking WebServer; acknowledged success two 100 ms beeps; error/no ACK three 100 ms beeps; maintenance shutdown one 300 ms beep. GPIO is forced LOW before final release.
