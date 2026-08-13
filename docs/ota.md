# OTA, validation, and rollback

## Confirmed build support

The target has 4 MB embedded flash. `partitions.csv` stays below that boundary:

| Partition | Offset | Size |
|---|---:|---:|
| NVS | `0x9000` | 20 KiB |
| OTA data | `0xE000` | 8 KiB |
| OTA slot 0 | `0x10000` | 1.8125 MiB |
| OTA slot 1 | `0x1E0000` | 1.8125 MiB |
| Core dump | `0x3B0000` | 64 KiB |

The installed PlatformIO Arduino-ESP32 SDK defines `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1` for ESP32-C3. `verifyRollbackLater()` is overridden because the core otherwise marks a pending image valid before Arduino `setup()`.

## Upload flow

1. Build `pio run -e esp32-c3-devkitm-1`.
2. In maintenance, upload `.pio/build/esp32-c3-devkitm-1/firmware.bin` or use multipart curl:

   ```bash
   curl -F 'firmware=@.pio/build/esp32-c3-devkitm-1/firmware.bin' \
     http://192.168.4.1/api/ota
   ```

3. `Update` writes and verifies the inactive application partition and selects it.
4. A one-shot marker in NVS (with RTC retained fallback) asks the next image to return to maintenance. It survives the software reset that intentionally starts a new measurement session.
5. The bootloader changes a new image to `PENDING_VERIFY`.
6. Firmware validates configuration, RTC structure, and the two-slot/OTA-data layout.
7. It calls `esp_ota_mark_app_valid_cancel_rollback()` only after those checks.

Sensor or gateway degradation does not invalidate otherwise sound software. A failed DHT, sonar, battery input, radio, or absent gateway is reported through health diagnostics.

If critical internal boot checks fail while pending, firmware calls `esp_ota_mark_app_invalid_rollback_and_reboot()`.

## Controlled rollback profile

`rollback-test` differs from production only by `GATHRA_ROLLBACK_TEST_FAIL=1`. When and only when it boots as `PENDING_VERIFY`, it restarts before validation. The bootloader should then mark it aborted and return to the previous valid slot.

```bash
pio run -e rollback-test
curl -F 'firmware=@.pio/build/rollback-test/firmware.bin' \
  http://192.168.4.1/api/ota
```

Never flash this profile as the final production image. It does not write the bootloader or partition table and does not deliberately corrupt flash. USB recovery remains available.

Actual rollback was observed on the connected ESP32-C3: the controlled image booted from `ota_0` as `PENDING_VERIFY`, restarted before validation, and the bootloader returned to the prior `production` image on `ota_1` in `VALID` state. Full evidence is recorded in `testing.md`.
