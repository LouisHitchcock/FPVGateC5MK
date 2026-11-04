# C5MK 1.2.5 installation

Choose c5zero for Waveshare ESP32-C5-Zero, or xiaoc5 for Seeed XIAO ESP32-C5. Each image embeds its board identity; link pins differ.

## Update through FPVGate

For an existing C5 firmware 1.1.0 or later with compatible two-slot partitions, open FPVGate Settings > System > C5 Receiver Firmware and select the matching `<board>-firmware.bin`. Only the application goes over UART. Confirm receiver returns online with version 1.2.5 and the expected board.

## First USB installation

Use esptool 5.1.0 or later. Close applications using the C5 port. Hold BOOT during connection if needed. Replace `<PORT>` with the C5 port, not the connected S3 gate port.

Waveshare:

```text
python -m esptool --chip esp32c5 --port <PORT> --baud 921600 write-flash --flash-mode dio --flash-freq 80m --flash-size 4MB 0x2000 c5zero-bootloader.bin 0x8000 c5zero-partitions.bin 0xf000 c5zero-ota_data_initial.bin 0x20000 c5zero-firmware.bin
```

XIAO:

```text
python -m esptool --chip esp32c5 --port <PORT> --baud 921600 write-flash --flash-mode dio --flash-freq 80m --flash-size 4MB 0x2000 xiaoc5-bootloader.bin 0x8000 xiaoc5-partitions.bin 0xf000 xiaoc5-ota_data_initial.bin 0x20000 xiaoc5-firmware.bin
```

No erase-all is required. Existing NVS calibration occupies 0x9000-0xefff. Firmware has two application slots at 0x20000 and0x200000, each 0x1e0000 bytes.

## Verification and recovery

Check SHA256SUMS.txt before flashing. After boot confirm board/version in FPVGate receiver status, live RSSI, all configured channels and a test race. Power-cycle acceptance must be recorded separately. An unconfirmed gate-linked OTA trial is designed to roll back after 60 seconds without its host; physical rollback has passed on the equivalent RC runtime; newly labelled image-byte smoke remains pending. If recovery is needed, use BOOT and USB to flash the previously qualified matching-board package.

Read QUALIFICATION.json and RELEASE_NOTES.md for validation scope and remaining hardware limits.
