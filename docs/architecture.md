# Node architecture

## State flow

```text
BOOT
  classify reboot marker, RTC alarm flag, then RTC timer flag
  -> POLL or MAINTENANCE

POLL
  acquire -> filter/verify -> persist sequence -> TELEMETRY
  -> matching ACK_COMMAND -> UTC/command -> COMMAND_RESULT
  -> persist history/state -> program and verify next wake
  -> release RTC flag -> external power removal

MAINTENANCE
  bounded dashboard/config/history/OTA
  -> fixed configured deadline, maximum 300 seconds
  -> persist -> program and verify next wake -> release RTC flag
```

`NodeApp` orchestrates isolated sensor, filter, protocol, command, RTC, storage,
buzzer, OTA, web, radio, and power modules.

## Boot and power

PCF8563 status is read before flag modification. Classification precedence is:

1. a valid persisted OTA or maintenance-reboot marker;
2. an alarm flag matching the pending UTC maintenance target;
3. the timer flag;
4. successful RTC communication with neither flag, indicating manual power-up;
5. unknown.

The active RTC flag remains asserted while firmware works. The final power-off
transaction persists state, programs and verifies the next timer and any
pending alarm, stops network/radio activity, completes buzzer output, and then
performs the only flag-release write. A verification failure retains the latch
and enters bounded recovery.

## Persistence

The `nvs_v2` partition contains validated configuration, persistent protocol
identity, filter memory, schedule and command state, reboot/deadline state,
power diagnostics, and measurement history. Sequence allocation commits the
next value before radio transmission.

History is a 512-slot circular store with 38-byte checked records and
alternating metadata generations. Corrupt records are skipped. Dashboard reads
use 12-entry pages or at most 100 sampled chart points.

## Sensors and filtering

The measurement path performs bounded echo checks, a configurable sonar burst,
median and MAD calculation, environment-compensated sound speed, Hampel-style
temporal filtering, rise/fall verification, and EMA. Filter memory survives
hard-power cycles. A suspicious raw change cannot replace the accepted distance
until confirmation policy permits it.

## Watchdog and OTA

The loop-task watchdog remains active. Sensor, I2C, radio, HTTP, and OTA work
has bounded waits and yields during long operations. Dashboard responses use
bounded data sets and a fixed socket deadline. OTA validates a pending image
only after required local configuration, storage, RTC, sensor, radio, history,
and partition checks succeed.
