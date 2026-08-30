# Node testing

## Automated checks

```bash
pio test -e native
pio run -e esp32-c3-devkitm-1
pio run -e hil
```

Native tests cover Protocol 3 golden bytes and rejection cases, commands and
idempotency, RTC calendar/timer/alarm logic, boot precedence, configuration,
persistent session/sequence safety, filtering, NVS history recovery, and the
bounded history pagination/chart algorithms.

The production build compiles the ESP32-C3 firmware. The `hil` build forces
maintenance mode while retaining real boot classification and peripherals.
Neither command flashes hardware.

## Dashboard bounded-response validation

With a full 512-record ring:

1. request status, configuration, and logs independently;
2. load the dashboard, a 100-point chart, and a 12-entry history page;
3. navigate all pages and verify chronological order;
4. keep the browser open for the complete maintenance lifetime;
5. connect a client that does not read a history response and verify the
   request closes within 2.5 seconds;
6. disconnect during a chart response, reconnect, and verify status remains
   responsive;
7. verify maintenance ends at its original deadline and no watchdog reset
   occurs.

## Hardware validation

Use the exact USB serial by-id path after mapping the board. USB can validate
register, state, radio, sensor, dashboard, and OTA logic, but it cannot prove
external power removal because USB powers the ESP32-C3.

Battery-only validation must confirm:

- the manual button establishes a verified RTC latch before it is released;
- maintenance remains available for the configured fixed lifetime;
- normal timer wakes transmit and then remove the switched rail;
- command and scheduled-alarm maintenance paths retain and release the correct
  RTC flags;
- OTA returns to maintenance, validates the selected slot, and preserves the
  remaining deadline;
- the configured poll interval is restored to the intended operational value.

Do not flash hardware, alter radio defaults, or change field configuration
without explicit operator authorization.

## Invariants

- Program and verify the next wake before clearing the power-holding flag.
- Persist `nextSequence` before transmission; gaps are allowed, reuse is not.
- Persist command receipt before applying it and persist the result before
  reporting `APPLIED`.
- Sensor, radio, or ACK failure must still lead to a bounded next-wake attempt.
- If next-wake verification fails, retain the latch and enter bounded recovery.
