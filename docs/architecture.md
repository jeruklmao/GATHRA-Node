# Node architecture

## State flow

~~~text
BOOT
  classify expected reboot marker, AF, then TF
  ├─ MANUAL_BUTTON -> establish 64 Hz TF latch -> MAINTENANCE
  ├─ RTC_SCHEDULED_MAINTENANCE -> keep AF asserted -> MAINTENANCE
  ├─ RTC_TIMER -> keep TF asserted -> POLL
  └─ OTA/MAINTENANCE_REBOOT -> keep existing latch -> MAINTENANCE

POLL
  acquire -> filter/verify -> persist sequence before TX
  -> TELEMETRY -> ACK_COMMAND -> UTC/command -> COMMAND_RESULT
  -> persist history/state -> program and verify next wake
  -> final flag release -> external power loss

MAINTENANCE
  bounded WebServer/OTA/config/history
  -> fixed deadline (maximum 300 seconds)
  -> persist -> program and verify next wake
  -> shutdown beep -> final flag release
~~~

Arduino setup and loop delegate to NodeApp. Sensor, filter, protocol, command, RTC, storage, buzzer, OTA, web, radio, and power responsibilities are isolated modules.

## Boot classification

PCF status is read before any flag modification. Precedence is:

1. a valid persisted OTA_REBOOT or MAINTENANCE_REBOOT marker;
2. AF plus a pending, valid UTC target matching current RTC time (0–300 seconds late);
3. TF;
4. successful PCF communication with neither flag means MANUAL_BUTTON;
5. UNKNOWN.

After this flag classification, an unconsumed persisted maintenanceActive state
overrides a non-maintenance result as MAINTENANCE_REBOOT recovery. A valid UTC
deadline resumes only its remaining time; an expired, missing, or untrusted
deadline receives a zero lifetime and proceeds directly to safe wake
reprogramming/release rather than starting another five minutes.

AF+TF therefore selects scheduled maintenance. A mismatched AF never masquerades as scheduled maintenance. Reboot markers are consumed only after classification and successful state initialization.

## Power invariant

TF or AF is the active, open-drain level latch. Reconfiguration preserves both flag bits through PCF8563 write-AND semantics. The centralized finalPowerOff transaction stops the portal, persists protocol/filter/history/command diagnostics, programs and verifies the next timer and any future alarm, sleeps SX1278, stops Wi-Fi, completes the buzzer, drives GPIO2 LOW, then performs the sole final Control_status_2 release write.

A completed AF-only maintenance wake defers AIE disable until that same final write. If next-wake readback fails, the current flag is not intentionally cleared and the Node enters a bounded recovery fault loop.

## Persistence

Hard power removal invalidates RTC_DATA_ATTR as application storage, so v2 uses NVS:

- gathra-config: schema-v2 operator configuration;
- gathra-state: checksummed persistent session, next sequence, filter memory, schedule, command, reboot/deadline and power diagnostics;
- gathra-history: fixed slots h000–h511 and alternating metadata copies.

Sequence allocation commits nextSequence before TX. History writes one 38-byte slot and one small alternating metadata record rather than rewriting a large blob. Corrupt records are skipped. A full ring overwrites the oldest slot.

Legacy configuration is read from the original NVS partition only when v2 configuration is absent. normalWakeIntervalSec is converted with ceil(seconds/60) and clamped to 1–255; useful Node, filtering, calibration, battery, LoRa, ACK, and maintenance values are retained.

## Sensors and filtering

The existing bounded architecture remains: physical echo checks, seven-ping burst, median, MAD, environment-compensated sound speed, Hampel-style temporal filtering, rise/fall verification, and EMA. Filter memory is persisted in NVS. A suspicious result completes fast verification while powered; any requested hard-power recheck is honestly rounded to the one-minute PCF minimum.

## Watchdog and recovery

The loop task watchdog remains enabled in polling and maintenance. Sensor/I2C/radio waits are bounded; long LoRa and streamed OTA operations explicitly feed the watchdog while yielding. OTA uses dual partitions and validates a pending image only after RTC, NVS, configuration, sensors, and radio initialize. A persisted maintenance deadline prevents reboot from restarting an unlimited five-minute window.

The irreducible limitation is hardware: a completely non-executing ESP cannot clear an already asserted PCF flag.
