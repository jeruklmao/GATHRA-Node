# Node testing

## Automated

Run:

~~~bash
pio test -e native
pio run -e esp32-c3-devkitm-1
pio run -e hil
~~~

Native tests cover exact big-endian v3 golden bytes, the appended `referenceDistanceMm` field including zero and uint32 limits, Protocol 1/2 rejection, malformed lengths/flags, all three packets and required commands, result codes, duplicate command idempotency and persistence order, PCF BCD/date/VL/timer/alarm logic, independent TF/AF clearing, preservation of an AF-only power latch until final release, alarm horizon, boot precedence, config conversion, persistent session/sequence power-loss safety, cold-boot flag-clear reconciliation, filter state, NVS history wrap/order/corruption/dual metadata recovery, and bounded history windows for empty, partial, full, and wrapped rings.

## Dashboard watchdog regression

Test a full 512-record ring with the following sequence:

1. keep the maintenance AP active without HTTP traffic as the control;
2. request status, config, and logs separately;
3. load `/`, one 100-point chart, and a 12-entry page;
4. navigate every page through all 512 records and verify chronological order;
5. leave the real browser dashboard open through the complete 300-second maintenance lifetime;
6. start a dashboard response from a client that does not read, and verify it is aborted within 2.5 seconds;
7. disconnect a chart client immediately, reconnect, and verify status still responds;
8. confirm there is no TASK_WDT reset and that maintenance still exits at its original fixed deadline.

The 2.1.0 forensic baseline reproduced the fault with only `GET /api/history`: status/config/log requests completed in 26–48 ms, while history advertised 10,106 bytes, returned zero bytes for 30 seconds, and triggered TASK_WDT. Firmware 2.1.1 must never use a watchdog-timeout increase as its acceptance criterion.

## Offline history recovery

Before debugging a field Node, stop the application in the ESP32-C3 ROM download loader and read the complete physical flash twice. Verify identical sizes and SHA-256 hashes, decode the partition table from offset `0x8000`, and independently read the actual `nvs_v2` offset/size. Run Espressif's `nvs_tool.py` on the copied NVS image, not the live device. In namespace `gathra-hist`, select the valid/newer `meta0` or `meta1` generation, calculate the oldest slot as `(head + 512 - count) % 512`, and validate every 38-byte record's magic, version, and FNV-1a checksum before exporting chronological CSV/JSON. Preserve the original images unchanged outside Git.

The physical evidence below is the Firmware 2.0.0 / Protocol 2 regression baseline. Firmware 2.1.0 / Protocol 3 must complete its own RF HIL before release; automated builds alone are not recorded as hardware evidence.

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
