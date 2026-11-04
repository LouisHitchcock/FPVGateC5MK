# C5MK v1.2.5

This release promotes the receiver behavior from 1.2.4-rc.1 to version 1.2.5.
It supports Waveshare ESP32-C5-Zero (`c5zero`) and Seeed XIAO ESP32-C5
(`xiaoc5`), using one board-specific image for FPVGate UART, native
RotorHazard USB and browser tools.

## Changes from stable 1.2.3

- Seats using the same frequency share one RF capture, filtered value and
  timestamp per scan cycle in UART and USB modes. Per-seat thresholds remain
  independent.
- Native RH merges multipath dips into one crossing, preserving peak timing.
- RH history retains the crossing peak so Marshal recalculation follows live
  peak timestamps with matching thresholds. Portable tests check agreement
  within the protocol's millisecond resolution; this is internal consistency,
  not a claim of millisecond physical RF accuracy.
- Adds offline receiver-comparison/timing-estimator tools and tests.

Receiver runtime sources match 1.2.4-rc.1 (`7f7631f`); the firmware version
changes to 1.2.5. The development raw-RSSI diagnostic changes are excluded.
No unqualified filtering or estimator replacement is adopted.

## FPVGate versus direct RotorHazard

Both use the same C5 capture/filtering source and shared-frequency behavior.
FPVGate's S3 applies an additional EMA before detecting peaks; direct C5 USB
has extra smoothing disabled by default. Host clock handling and timestamp
resolution also differ. These complete paths are not guaranteed to produce
identical crossing times. Calibrate each installation's thresholds.

## Installing

Use the files for your exact board. For a complete USB flash, place the four
board-prefixed assets in one directory and run the following, substituting
`c5zero` or `xiaoc5` for BOARD and your port for PORT:

```text
esptool --chip esp32c5 --port PORT --baud 921600 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x2000 BOARD-bootloader.bin 0x8000 BOARD-partitions.bin 0xf000 BOARD-ota_data_initial.bin 0x20000 BOARD-firmware.bin
```

Close RH, browser USB connections and serial monitors before flashing.
Firmware 1.1.0 or later supports application-only updates through FPVGate's
C5 Receiver Firmware page. Use only the matching BOARD-firmware.bin. Earlier
versions require the complete USB installation. The 1.1.0 partition layout
is retained. Upgrading a native USB installation from before 1.2.2 should
include the bootloader. Do not use RH's STM32 node updater.

FPVGate gate rc.2 observes the receiver's 60-second trial before reporting
update completion; older gate versions can report success before a later
rollback. Preserve settings and a recovery path before updating.

## Validation and limits

Portable protocol, detector, host and RH tests and both clean production
builds are recorded in QUALIFICATION.json and SOURCE_MANIFEST.json. Packaged
images are checked for chip/board/version, partition offsets, two OTA slots,
image fit, reserved SRAM and SHA-256.

Physical evidence on the equivalent 1.2.4-rc.1 runtime includes linked OTA,
failed-trial rollback and production-image restoration on both boards,
automatic gate race saves using manual laps, and about 1037/1033 samples
per slot per second with eight distinct configured channels. It does not
constitute physical acceptance of the newly labelled 1.2.5 image bytes.

An eight-hour gate rc.2 plus receiver rc.1 endurance run is still in progress
at preparation time; results must be read separately. Independent
multi-transmitter RF/absolute timing, physical power-cut recovery, live
native USB/RH race acceptance and XIAO USB coverage remain incomplete.
Earlier native Waveshare discovery/local timing evidence is historical.

Gate hardware self-test concurrent with C5 telemetry produced sequence gaps
and a bad record on each tested board. It includes flash writes; exact cause
remains under investigation. Do not run diagnostics while timing races.
The 15-character receiver version field used by FPVGate supports 1.2.5.

SHA256SUMS.txt covers the board-prefixed release images. Source and build
provenance accompany the assets. Version 1.2.4 was not released as stable;
this release follows stable 1.2.3 and prerelease 1.2.4-rc.1.
