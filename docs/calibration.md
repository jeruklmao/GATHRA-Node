# Calibration

Calibration is an operator action in the maintenance dashboard. Place the
monitored surface at the installation datum, run **Measure Now** until the
accepted distance is stable, then capture that value or enter
`referenceDistanceMm` explicitly.

The Node derives:

```text
waterHeightMm = referenceDistanceMm - acceptedDistanceMm
```

A zero reference means calibration is unset. Water height is then unavailable
and the calibration health flag is set; firmware does not invent a zero height.

The production measurement path uses physical echo validation, a seven-ping
median/MAD burst, DHT-based sound-speed compensation, Hampel filtering,
rise/fall confirmation, and EMA. The accepted distance and filter baseline are
persisted in NVS across RTC-controlled hard-power cycles.
