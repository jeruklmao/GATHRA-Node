# Node testing

## Automated

Run:

~~~bash
pio test -e native
pio run -e esp32-c3-devkitm-1
pio run -e hil
~~~

Native tests cover exact big-endian v2 golden bytes, malformed versions/lengths/flags, all three packets and required commands, result codes, duplicate command idempotency and persistence order, PCF BCD/date/VL/timer/alarm logic, independent TF/AF clearing, preservation of an AF-only power latch until final release, alarm horizon, boot precedence, config conversion, persistent session/sequence power-loss safety, cold-boot flag-clear reconciliation, filter state, and NVS history wrap/order/corruption/dual metadata recovery.

## USB HIL evidence (2026-08-25)

Board identity was positively mapped as USB MAC 10:00:3B:D4:BC:FC. PCF8563 responded at 0x51 on GPIO8/9 with internal pull-ups; repeated reads were stable, the clock advanced, INVALID_VL was reported until a trusted ACK wrote UTC, and subsequent drift was 0–1 second. A 64 Hz manual latch asserted TF and read back in level mode. A minute/hour/day alarm asserted AF; TF+AF read 0x0F and classified RTC_SCHEDULED_MAINTENANCE.

Sensor acquisition produced 7/7 valid echoes, median 1039 mm, MAD 1 mm, 32.0 C, 72.9 percent humidity, and 4029 mV battery. One NVS history entry and filter baseline survived resets/OTA. Protocol 2 RF was acknowledged on the first attempt. SET_POLL_INTERVAL_MINUTES=5, SCHEDULE_MAINTENANCE_AT, and ENTER_MAINTENANCE_NOW each returned APPLIED and were confirmed by Gateway COMMAND_RESULT.

Node browser OTA streamed 971296 bytes, rebooted with TF retained, classified OTA_REBOOT, booted ota_1 PENDING_VERIFY, and marked it valid after checks. MAINTENANCE_REBOOT classification also passed.

USB keeping the ESP alive after flag clear is explicitly not a hard-power PASS.

## Battery-only HIL procedure

1. flash the production image and ensure Gateway remains connected;
2. unplug only Node USB;
3. press and hold the physical Node button;
4. wait for two short latch-confirmation beeps, then release;
5. connect Intel AX211—not RTL8188—to the Node AP;
6. confirm MANUAL_BUTTON, TF latch, persistent state/history and five-minute deadline;
7. observe the shutdown beep, AP disappearance, and physical power loss;
8. observe a later RTC_TIMER telemetry packet and a second power loss;
9. repeat for ENTER_MAINTENANCE_NOW and a minute-aligned scheduled AF wake;
10. restore poll interval to 10 minutes.

Persisted power-event diagnostics are the evidence source when USB serial is absent.

## Battery-only HIL evidence (2026-08-25)

With Node USB removed, one reset serviced a timer that had become due while USB
masked the earlier power release. Thereafter the physical rail and LED switched
off after every poll and the PCF timer restarted the Node without a button.
Gateway observed sequences 13 through 18 with unchanged session B89AAF6A,
`RTC_TIMER`, healthy 7/7 measurements, acknowledgements, and approximately
one-minute cadence.

A physical press-and-hold produced two 100 ms latch beeps; after release the LED
and AP remained on and the non-blocking three-second indicator continued. The
dashboard reported `MANUAL_BUTTON`, TF/TIE level latch, the original fixed
300-second deadline, seven persistent history entries, and preserved filter and
calibration state. At 299.75 seconds the AP was still present; it disappeared at
about 300 seconds, the rail switched off, and sequence 18 later arrived as
`RTC_TIMER`.

RF `ENTER_MAINTENANCE_NOW` command 7 kept TF asserted and exposed the AP with
source `ACK_COMMAND`. A battery-powered browser OTA retained the latch, rebooted
as `OTA_REBOOT`, marked the new slot valid, and preserved the original deadline.
Scheduled target 1787632140 was armed with AIE while ordinary sequences 21 and
22 continued. At the target, AF+TF was 0x0F and the dashboard reported
`RTC_SCHEDULED_MAINTENANCE`; after 300 seconds the one-shot completed, AF/AIE
were released, and sequence 23 reported `scheduleState=COMPLETED`.

The final image was installed by OTA, interval command 10 restored the production
default of 10 minutes, and a final manual cold boot reconciled the previous
battery release to `flagClearSucceeded=true`. History count was 12 and the
session remained B89AAF6A.

## Invariants

- Never clear the power-holding flag until a next wake is programmed and verified.
- Persist nextSequence before TX; gaps are allowed, reuse is not.
- Persist command receipt before applying and result before reporting APPLIED.
- Missing sensor/radio/ACK still schedules the next wake and releases power.
- If next-wake verification fails, retain the latch and enter bounded recovery.
