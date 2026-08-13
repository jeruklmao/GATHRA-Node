# Installation calibration

The factory/default configuration has no mounting reference and no installation distance limits. This is deliberate: mounting geometry is site-specific.

Without calibration, the node still reports raw and accepted sensor-to-surface distance, echo duration, MAD, and quality. `CALIBRATION_MISSING` is set and derived water height is null/unavailable.

## Reference workflow

1. Put the node in maintenance mode and inspect multiple measurements/history.
2. Ensure the intended installation datum is actually below the sensor and readings are stable.
3. Either enter a measured `referenceDistanceMm`, or explicitly press **Capture Current as Reference**.
4. Confirm the displayed reference and derived result.

Capture is rejected unless a current measurement is `STABLE`, initially `ACCEPTED`, or `CHANGE_CONFIRMED`. A transient, uncertain, invalid, or merely old baseline cannot be captured as if it were the current surface.

The dashboard warning is intentional: only capture when the current target/surface represents the installation datum. Derived signed height is `reference distance - accepted distance`; negative values are preserved rather than silently clamped.

Reference validation accepts zero (unconfigured) or 20–30000 mm. These broad bounds protect representation/driver assumptions; they are not a claim about a particular mounting geometry.

## Installation limits

`installationMinimumDistanceMm` and `installationMaximumDistanceMm` must both be zero (disabled) or a valid ordered pair within 20–30000 mm. When enabled, pings outside the explicit envelope are marked invalid before the burst median. Do not enable limits until the installation has been measured.

## Battery calibration

The divider's nominal ratio is three. `batteryCalibrationFactor` (0.5–1.5) and offset (-1000 to +1000 mV) correct the final battery estimate. Compare the battery terminals with a trusted meter before changing these values. The 3500/3300 mV flags are field-tuning defaults, not charge/discharge control and not a declaration of battery chemistry.
