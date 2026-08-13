# Firmware architecture

## Orchestrator

`NodeApp` owns the application lifecycle. Arduino `setup()` and `loop()` only delegate to it. Sensor, radio, web, and power modules do not run independent periodic application tasks.

```text
                         GPIO/button or OTA one-shot
                                    │
                                    ▼
BOOT ─ timer/cold ─→ ACQUIRE → FILTER ─ plausible ─→ TRANSMIT → SLEEP
                                  │                       │
                                  └ suspicious → VERIFY ─┘
                                    │
                                    ├ transient: preserve accepted baseline
                                    ├ confirmed: update post-verification EMA
                                    └ uncertain: preserve baseline + sooner wake

MAINTENANCE: SoftAP + WebServer + diagnostics/config/calibration/OTA
```

Blocking is bounded and intentional at peripheral transaction boundaries. The SRF05 uses an any-edge GPIO ISR and FreeRTOS task notification for echo completion. DHT acquisition, echo capture, and LoRa interrupt windows never overlap in the normal sequence. ESP/Arduino networking continues to use its internal tasks.

## Module responsibilities

- `lib/app`: lifecycle, state transitions, health aggregation, telemetry orchestration.
- `lib/config` and `lib/config_storage`: pure configuration validation and NVS persistence.
- `lib/dht22`, `lib/srf05`, `lib/battery`, `lib/sensors`: production acquisition drivers and ordered sensor facade.
- `lib/filtering`: burst median/MAD and temporal candidate verification/EMA.
- `lib/history`: versioned retained filter state and 64-entry compact ring.
- `lib/protocol`: explicit binary v1 telemetry and ACK codecs.
- `lib/radio`: bounded RadioLib TX/RX/ACK retries.
- `lib/maintenance` and `lib/dashboard`: local WebServer API and compiled vanilla HTML/CSS/JS.
- `lib/ota`: deferred application validation and official ESP OTA state APIs.
- `lib/power`: button debounce/long press, wake reason, Wi-Fi shutdown, wake-source setup.
- `lib/logging`: serial plus bounded 64-line in-RAM circular log.

## Measurement integrity

The burst layer validates idle echo level, rise/fall completion, pulse duration, sample count, and optional explicit installation limits. It calculates the median and MAD across valid pings. DHT compensation uses `c = 331.3 + 0.606T + 0.0124RH`; the fallback uses the conventional SRF05 reference speed (344.8 m/s, equivalent to approximately `/58` in centimetres).

Temporal filtering compares a burst with a rolling accepted median/MAD using `max(K × 1.4826 × MAD, absoluteFloor)`, and separately checks sudden change from the last accepted value. `MAD == 0` therefore remains well-defined. Suspicious readings become candidates rather than being discarded or accepted.

Verification counts the initial candidate as observation one. Candidate votes must remain within the configured tolerance. A majority return to the accepted baseline is transient-rejected; insufficient evidence in either direction is explicitly uncertain. EMA never sees an unverified candidate. A confirmed persistent regime change atomically reseeds the EMA and rolling Hampel baseline at the verified candidate median, avoiding contamination from the old surface regime.

Smaller sensor-to-surface distance is treated as apparent rise and uses the faster policy. Both rise and fall are verified.

## Retention and identity

`RtcRetainedState` is magic/version/size checked and compile-time limited below 4 KiB. It retains accepted filter history, EMA, candidate diagnostics, boot/session ID, sequence, and 64 compact history entries. A cold/non-deep-sleep reset creates a new random session and resets retained history. Deep-sleep wake retains both session and sequence.

NVS is reserved for validated operator configuration. It is not used for measurement history or text logs.

GPIO2 is sampled by the debouncer and also has a minimal edge ISR that latches a completed debounced press while a bounded sensor/radio call is active. The orchestrator consumes that request only at a safe transaction boundary. Deep-sleep entry still waits for release before arming active-low wake.

## Failure behavior

Recoverable failures never enter a permanent halt. Sonar/DHT/battery/radio errors set health flags and continue to bounded transmit/sleep behavior. Missing gateway ACK completes after at most three attempts. A genuinely unsafe pending OTA boot validation failure invokes official rollback.
