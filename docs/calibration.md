# Calibration

Calibration remains an operator action in the maintenance dashboard. Place the monitored surface at the installation datum, run Measure Now until the accepted value is stable, then capture the current accepted distance or enter referenceDistanceMm explicitly.

Derived water height is referenceDistanceMm minus acceptedDistanceMm. A missing reference produces the documented unavailable height sentinel and health flag; it does not invent a zero height.

Protocol/config v1 migration preserves reference distance, installation range, battery factor/offset/thresholds, sensor filtering, and LoRa/ACK settings when the legacy record is valid. Schema-v2 configuration lives in nvs_v2 and survives complete hard-power cycles and OTA.

The seven-ping median/MAD, physical echo validation, DHT-based sound-speed compensation, Hampel history, rise/fall confirmation, and EMA remain the production measurement path. Their temporal baseline is now persisted in NVS, so obstacle filtering does not restart after every RTC-controlled power cycle.
