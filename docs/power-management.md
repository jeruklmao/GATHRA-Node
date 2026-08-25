# Hard-power management

The PCF8563 is battery-powered continuously. ESP32-C3, SX1278, sensors, and boost circuitry are behind an AO3401A P-channel MOSFET. A 100 kΩ resistor pulls its gate high (off); the manual button or PCF8563 open-drain INT pulls it low (on). The ESP has no gate or button GPIO.

INT uses level mode (TI_TP=0). TIE+TF holds a timer boot; AIE+AF holds a scheduled-alarm boot. Flags are deliberately left set during work.

## Manual latch

A manual cold-power boot has neither AF nor TF. Firmware configures the timer at 64 Hz with value 8 (about 125 ms), TIE enabled, and level mode. It polls until TF and the control bits read back, with a one-second bound. Only then it emits two 100 ms beeps and declares that the operator can release the button.

## Normal timer

Poll interval is stored as 1–255 minutes, default 10. Normal wakes use source 1/60 Hz and the configured count. A future maintenance alarm remains armed simultaneously.

## Final transaction

1. stop accepting maintenance operations;
2. persist state, filter, history, command, and diagnostics;
3. program the normal timer while preserving TF/AF;
4. preserve and verify a pending alarm, or defer completed active-alarm disable;
5. validate timer/alarm readback;
6. sleep radio and stop Wi-Fi;
7. flush logs and emit one 300 ms maintenance shutdown beep when applicable;
8. drive buzzer LOW;
9. atomically clear only active release flags and disable AIE for a completed alarm;
10. expect external power loss.

Step 9 is the point of no return. Failure before it retains the current latch and enters recovery. USB may keep the ESP running after step 9 and is not evidence of hard power-off.

On battery power, a successful step 9 normally removes the rail before a final
NVS confirmation write can complete. The event is stored first with
`flagClearAttempted=true`; the next genuine ESP `POWER_ON` boot reconciles the
prior record to `flagClearSucceeded=true`. Software, watchdog, and OTA resets do
not count as that independent evidence.

If AF becomes due during a TF-held operation, AF is preserved and a scheduled maintenance lifetime begins; it is not discarded during normal timer rearming.
