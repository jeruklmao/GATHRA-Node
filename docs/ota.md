# Node OTA and rollback

Browser OTA is available only in maintenance mode and writes the inactive partition. The 4 MiB layout contains ota_0 and ota_1 at 0x10000 and 0x190000, each 0x180000 bytes, with otadata at 0xE000.

Before accepting a reboot, firmware verifies that a PCF level latch is active, persists expectedReboot=OTA_REBOOT, retains the maintenance active/deadline state, and leaves TF/AF untouched. Upload chunks service the enabled loop watchdog without disabling protection.

On reboot, the marker takes precedence over AF/TF and selects OTA_REBOOT maintenance. A PENDING_VERIFY image is marked valid only after configuration, v2 NVS, PCF communication, history, sensors and SX1278 initialization pass. ESP-IDF rollback remains enabled; the rollback-test build deliberately fails validation and must roll back to the other valid slot.

If OTA occurs during an AF-held scheduled-maintenance window, the visible boot
reason remains `OTA_REBOOT` as required by precedence. At shutdown the firmware
re-evaluates the retained AF and authoritative UTC target without the reboot
override, completes the one-shot schedule, and only then clears AF.

A maintenance dashboard reboot uses the same flow with MAINTENANCE_REBOOT. The marker is consumed after classification; the persisted UTC deadline prevents either reboot from restarting an unlimited maintenance period.

USB OTA validates register/state/reboot logic only. A battery-only test is needed to prove that the MOSFET latch remains asserted across reset and can still be released afterward.
