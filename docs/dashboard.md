# Maintenance dashboard and HTTP API

Maintenance mode starts `GATHRA-NODE-<node-id suffix>` with WPA2 password `sman35jakarta` and serves the offline dashboard at `http://192.168.4.1/`. All HTML, CSS, JavaScript, and canvas graph code is compiled into firmware. There are no external assets.

The page exposes:

- firmware/build, immutable MAC, logical ID, reset/wake reasons, uptime/state
- battery ADC/voltage and health flags
- DHT freshness and acoustic-compensation state
- raw echo/distance, accepted distance, MAD, sample counts, candidate/filter state
- LoRa readiness, radio code, attempts, and ACK result
- RTC session/sequence/history utilization and a selectable graph
- every runtime `NodeConfig` field with units and server-side validation
- manual and capture calibration
- bounded recent logs, radio test, browser OTA, reboot, and exit

## Routes

| Method | Route | Purpose | Refreshes inactivity? |
|---|---|---|---|
| GET | `/` | dashboard | yes |
| GET | `/api/status` | complete device JSON | no |
| GET | `/api/history` | ordered RTC history | no |
| GET | `/api/config` | runtime configuration | no |
| PUT | `/api/config` | validate/apply/save candidate | yes |
| POST | `/api/measure` | production acquisition/filter pipeline | yes |
| POST | `/api/calibration/capture` | stable accepted distance as reference | yes |
| POST | `/api/calibration/set` | manual reference or zero to clear | yes |
| POST | `/api/radio/test` | bounded telemetry/ACK test | yes |
| GET | `/api/logs` | bounded current-boot log ring | no |
| POST | `/api/activity` | throttled real user-interaction heartbeat | yes |
| POST | `/api/ota` | multipart `.bin` upload | yes during upload |
| POST | `/api/reboot` | reboot and return to maintenance once | yes |
| POST | `/api/maintenance/exit` | AP off and sleep | yes |

The browser only emits `/api/activity` after pointer, touch, or keyboard interaction, throttled to 15 seconds. Automatic polling cannot keep maintenance alive forever.

Configuration is candidate-driven. Invalid values return HTTP 422. A candidate radio change must initialize successfully before NVS is updated; failure restores the prior radio configuration and returns HTTP 409.

Reboot and exit are deferred briefly after their JSON response so a field browser can receive confirmation before the AP is stopped. OTA activity uses the same bounded reboot scheduling.

The graph uses sequence as its X axis because no external RTC supplies trustworthy wall-clock time. Null values remain gaps, and transient/uncertain points are red.
