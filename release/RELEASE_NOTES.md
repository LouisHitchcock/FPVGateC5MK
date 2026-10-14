# C5MK 1.2.0: direct USB timing with RotorHazard

C5MK can now connect directly to the computer running RotorHazard over USB,
without an ESP32-S3, Wi-Fi connection, FPVGate plugin or RH core changes.

## What's new

- One C5 exposes eight native RH serial receivers with independent frequency
  and EnterAt/ExitAt controls, live RSSI and standard Sensor Tuning capture.
- RH's revision request selects USB timing mode automatically. The same
  board-specific firmware retains the existing S3/UART receiver connection;
  there is no separate USB-only image.
- Crossing detection uses every successful scan sample. Passes are timed at
  the midpoint of their highest RSSI plateau on the C5 clock and reported
  through RH's API 35 serial-node protocol.
- USB ownership does not take the radio from an active S3. Five seconds
  without valid RH requests releases it. Invalid frames and stale samples
  cannot complete an old crossing.

## Installing

Choose `c5zero` for Waveshare ESP32-C5-Zero or `xiaoc5` for Seeed XIAO ESP32-C5.
The files below are prefixed with those board names. Each board needs its
matching firmware; the UART pins differ.

A C5 on 1.0 or earlier needs the full USB flash because 1.1.0 changed the
partition layout. Rename the downloaded board-prefixed files to the names
below, or use the unprefixed files in `release/<board>/` at the release tag:

```text
esptool --chip esp32c5 --port <port> --baud 921600 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x2000 bootloader.bin 0x8000 partitions.bin 0xf000 ota_data_initial.bin 0x20000 firmware.bin
```

1.2.0 retains the 1.1.0 partition layout. Existing 1.1.0 receivers can update
through FPVGate's C5 Receiver Firmware page over the UART link. Close any RH
or browser serial connection before a USB flash. Do not use RH's STM32 node
updater on the C5.

For direct USB, add the C5's port under **Advanced Settings > Hardware > Serial
Ports** in RH and restart RH. Calibrate each enabled receiver in Sensor Tuning.
The port is configured once; this release does not add automatic OS port
registration or change RH's unplug/replug recovery. Don't use USB RH and an
S3 to control the same receiver simultaneously.

[USB setup and limitations](https://github.com/LouisHitchcock/FPVGateC5MK/blob/v1.2.0/docs/USB_ROTORHAZARD.md)
and [flashing instructions](https://github.com/LouisHitchcock/FPVGateC5MK/blob/v1.2.0/release/README.md).

## Validation and limitations

- 160 USB protocol checks and four integration tests using stock RH's actual
  serial-node client passed.
- Existing meter, UART/node and reference-host suites passed: 60, 1201 and 48
  checks respectively.
- Both board firmware builds passed their SRAM guard checks.
- Waveshare C5-Zero was flashed with the development version of this code:
  eight R1-R8 receivers and live RSSI were verified through RH. The tester
  then reported timing working in the local RH setup.
- Physical XIAO USB testing is still pending. Eight-pilot RF detection,
  unplug/replug behaviour and timing accuracy against video are not validated
  by these checks.

USB reports the existing C5 filtered RSSI, scaled to RH's valid 1-254 range.
It does not add Widscan58's 255 ms median filter or send the full 1 kHz trace
to Marshal. Only the latest pass per receiver is retained between host reads.
Recalibrate thresholds for your receiver position and test before an event.
