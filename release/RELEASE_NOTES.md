# C5MK 1.2.3: fast USB signals and browser timing

This release puts the browser USB RSSI stream on the short hop-and-capture
path already used for FPVGate's eight-pilot timing. The UART link,
RotorHazard endpoint and partition layout are unchanged from 1.2.2.

## Firmware

- USB scanning defaults to `scanmode fast`, using `noden` (256 samples)
  and `nodeskip`, instead of the slower diagnostic scan settings.
- Fast mode measures clipping and gain diagnostics every sixteenth cycle.
  Other records contain power only: clear diagnostic flags between those
  cycles do not prove clipping has stopped.
- Fast mode does not wait for USB buffer space. Slow-host losses increment
  the existing drop counter instead of slowing the RF scan.
- `scanmode full` restores the original USB scan, using `n`, `settle`
  and `tune`, with full diagnostics on every reading. `status` shows the mode.

Older firmware can connect to the browser tools; install 1.2.3 for the fast
USB path. Roughly 1000 readings per pilot per second was measured on the
node scan, not guaranteed for every browser: delivery depends on the USB
host, settings and pilot count.

## Browser tools

- [Dashboard](https://c5mk.fpvgate.xyz/): eight-pilot traces and peak hold,
  one large waterfall, expandable RSSI history and optional full-band sweeps.
  Sweeps pause pilot history and restore channels afterwards; the default
  live view uses incoming pilot readings without retuning the receiver.
- [Lap timer](https://c5mk.fpvgate.xyz/timer/): practice/race modes for
  1-4 pilots, voice, CSV results, optional RSSI graphs and help, and advanced
  entry/exit levels that can be typed or dragged on touch screens.
- Mobile-first controls, wider desktop layouts, FPVGate branding and
  self-hosted fonts, plus connection and troubleshooting guidance.
- Desktop Chrome/Edge uses Web Serial; Android Chrome uses WebUSB.
  Physical Android testing is pending. iPhone/iPad USB is not supported.

Website updates deploy separately through GitHub Pages. Until deployment
finishes, the public site can still show an older interface.

## Installing

Choose `c5zero` for Waveshare ESP32-C5-Zero or `xiaoc5` for Seeed XIAO
ESP32-C5. Use [the web flasher](https://fpvgate.xyz/c5.html#flasher), selecting
Multi Pilot, or download the matching board-prefixed assets below.
The boards have different UART pins: do not interchange their images.

For a complete USB flash, rename the four files to these unprefixed names
(or use `release/<board>/` at this tag):

```text
esptool --chip esp32c5 --port <port> --baud 921600 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x2000 bootloader.bin 0x8000 partitions.bin 0xf000 ota_data_initial.bin 0x20000 firmware.bin
```

Close RotorHazard, serial monitors and browser USB connections first, then
unplug/reconnect afterwards. Do not use RotorHazard's STM32 node updater.
A C5 on 1.1.0 or later can receive `firmware.bin` through FPVGate's C5
Receiver Firmware page. A C5 on 1.0 or earlier needs the complete USB flash.
Application-only updates do not update the bootloader: use a complete USB
flash when upgrading from before 1.2.2 for Linux/Raspberry Pi RotorHazard.

See [flashing instructions](https://github.com/LouisHitchcock/FPVGateC5MK/blob/v1.2.3/release/README.md)
and [RotorHazard USB setup](https://github.com/LouisHitchcock/FPVGateC5MK/blob/v1.2.3/docs/USB_ROTORHAZARD.md).

## Validation and limitations

Portable tests pass: 60 meter/framing checks, 1201 node checks, 48 host
checks, 305 RotorHazard USB checks and eleven USB parser scenarios.
Browser lap detection, graph, spectrum and transport tests pass. Browser
layouts were checked from 320 to 1920 pixels with simulated receiver data.
Both board images build with pinned ESP-IDF 5.5.1 and pass the SRAM guard.
Release assets include SHA-256 checksums.

No new physical-device flash or race test was performed for this release.
Previous Waveshare USB and XIAO UART validation is recorded in the
[1.2.2 notes](https://github.com/LouisHitchcock/FPVGateC5MK/releases/tag/v1.2.2);
it is not hardware validation of these binaries. Physical XIAO USB, Android
USB and multi-pilot race validation remain pending. Check reception,
thresholds and lap detection before using this release at a race.
