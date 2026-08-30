# Node maintenance dashboard

Maintenance mode opens a node-local WPA2 access point and serves its dashboard
at `http://192.168.4.1/`. The dashboard has no additional login, must not be
bridged to an untrusted network, and never sends credentials in status JSON.

The configured maintenance lifetime is 60–300 seconds and defaults to 300
seconds. Passive status, history, and log reads do not extend the deadline.

## Current functions

The dashboard shows firmware **2.1.1**, Protocol **3**, Node and boot identity,
maintenance deadline, PCF8563/RTC state, next poll and maintenance schedule,
persistent sequence and command state, sensor/calibration values, LoRa result,
history utilization, OTA state, and power diagnostics.

Operator actions include:

- run the production measurement and filtering path;
- capture, set, or clear the reference distance;
- validate and save sensor, filtering, battery, radio, ACK, poll, and
  maintenance configuration;
- send a real Protocol 3 test packet;
- install OTA firmware, reboot while preserving maintenance, or finish
  maintenance through the verified power-off transaction.

Configuration changes are validated before persistence. Radio settings are
tested before they replace the active settings.

## Bounded history API

`GET /api/history?offset=0&limit=12` returns chronological full records. The
default and maximum page size are both 12; larger limits are clamped. The
response contains `count`, `capacity`, `offset`, `limit`, `returned`,
`hasPrevious`, `hasNext`, nullable `nextOffset`, and `entries`. Previous and
Next controls request only the selected page.

`GET /api/history/chart?series=rawDistanceMm&maxPoints=100` returns compact
chronological chart entries. The default and maximum are 100 points. When the
ring is larger, points are distributed uniformly and retain the earliest and
latest endpoints. Supported series are:

- `rawDistanceMm`
- `acceptedDistanceMm`
- `waterHeightMm`
- `temperatureC`
- `humidityPercent`
- `batteryMv`

Chart entries use `s` for sequence, nullable `v` for value, and numeric `f` for
filter state. Both history endpoints have bounded NVS work and JSON size.
Responses are written in 512-byte chunks with a 2.5-second request deadline.
The dashboard loads history on entry or explicit refresh; status polling never
scans the history ring.

## Buzzer

GPIO2 is active HIGH. Current patterns are one short poll-start beep, two short
manual-latch beeps, a short maintenance indicator every three seconds, two
short success beeps, three short error/no-ACK beeps, and one longer maintenance
shutdown beep. Firmware drives the pin LOW before releasing external power.
