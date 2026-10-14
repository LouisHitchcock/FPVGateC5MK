# C5MK 1.2.1: steadier RSSI

A receiver fix: readings no longer spike after hops, and a steady signal
holds still instead of wandering. Thanks to RaceFPV, who found the cause
([#1](https://github.com/LouisHitchcock/FPVGateC5MK/pull/1)).

## What's changed

- The C5 used to rewrite its fixed receive gain after every hop, even when it
  was already set, and each rewrite briefly disturbed the receiver. It now
  checks the gain first and writes it only if it has changed or been cleared.
- Through FPVGate at about 1030 readings per second per pilot (XIAO
  ESP32-C5, 8 Raceband pilots, VTX off): R1's largest false bump fell from
  224 to 13.5 RSSI counts, and its spread from 24 to 3.9, the same as the
  other seven channels.
- A VTX on R6 read about 5 dB stronger and about three times steadier
  (spread 14.1 to 5.2 counts).
- On the C5-Zero's USB scanner, raw readings more than 6 dB over the noise
  floor fell from 61 to 3 in about 97,000.
- No change in rate, timing, gaps or the noise floor.
- The build is pinned to pioarduino platform 55.03.35 (ESP-IDF 5.5.1)
  instead of the moving "stable" release.

## Recalibrate after updating

A steady signal now reads about 3-6 dB higher than on 1.2.0 and earlier.
EnterAt/ExitAt thresholds and gate calibrations made on older firmware will
sit low: run auto-calibrate in FPVGate, or Sensor Tuning in RotorHazard,
again after updating.

## Installing

Choose `c5zero` for Waveshare ESP32-C5-Zero or `xiaoc5` for Seeed XIAO ESP32-C5.
The files below are prefixed with those board names. Each board needs its
matching firmware; the UART pins differ.

A C5 already on 1.1.0 or 1.2.0 can update through FPVGate's C5 Receiver
Firmware page over the UART link: 1.2.1 keeps the 1.1.0 partition layout.

A C5 on 1.0 or earlier needs the full USB flash because 1.1.0 changed the
partition layout. Rename the downloaded board-prefixed files to the names
below, or use the unprefixed files in `release/<board>/` at the release tag:

```text
esptool --chip esp32c5 --port <port> --baud 921600 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x2000 bootloader.bin 0x8000 partitions.bin 0xf000 ota_data_initial.bin 0x20000 firmware.bin
```

Close any RH or browser serial connection before a USB flash. Do not use RH's
STM32 node updater on the C5.

[Flashing instructions](https://github.com/LouisHitchcock/FPVGateC5MK/blob/v1.2.1/release/README.md),
[USB setup for RotorHazard](https://github.com/LouisHitchcock/FPVGateC5MK/blob/v1.2.1/docs/USB_ROTORHAZARD.md)
and [the measurements](https://github.com/LouisHitchcock/FPVGateC5MK/blob/v1.2.1/docs/RF_RESEARCH.md#3-post-hop-spikes).

## Validation and limitations

- Meter, UART/node, reference-host and USB protocol suites passed: 60, 1201,
  48 and 160 checks.
- Both board firmware builds passed their SRAM guard checks.
- Tested on a Waveshare C5-Zero (gain 30) and on a XIAO ESP32-C5 behind a
  XIAO S3 running FPVGate (gain 31), with and without a VTX on R6; updated
  over the link and back.
- Tested at gains 30 and 31 only. Direct USB to RotorHazard runs with the
  change but the RSSI comparison there was inconclusive and is being
  repeated.
