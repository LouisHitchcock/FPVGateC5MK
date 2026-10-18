# C5MK V1.2.2-RC.3: recover RotorHazard USB discovery

Release candidate for Waveshare ESP32-C5-Zero and Seeed XIAO ESP32-C5.
Includes the USB timing improvements from 1.2.2-rc.1 and fixes discovery after
unexpected serial input. The UART link and 1.1.0 partition layout are unchanged.

## USB discovery fix

An unfinished console line could prevent the C5 from recognising RotorHazard's
revision request. RH then reported "Unable to fetch revision code for serial
node". Restarting RH left the partial line intact, so discovery kept failing
even though the C5 had finished booting.

The revision request can now claim USB after partial or overflowed console
input. Ordinary console commands still work, and an active S3/UART controller
still prevents USB takeover.

This fixes a reproduced failure mechanism. It does not establish what sent
unexpected bytes on the reported Raspberry Pi installations.

## Included from 1.2.2-rc.1

- RH retains its USB session while idle, until a console command requests it.
- Node API 36, frequency verification and disabled seats are supported.
- Live RSSI and history retain short peaks between RH polls.
- Frequency-change settling and brief VTX start-up burst rejection reduce blips.
- The crossing indicator stays visible for 250 ms after a short pass.

## Installing

Choose `xiaoc5` for the Seeed XIAO ESP32-C5 or `c5zero` for the Waveshare
ESP32-C5-Zero. Download that board's four prefixed binaries and rename them to
the unprefixed names below, or use [the web flasher](https://fpvgate.xyz/c5.html).

```text
esptool --chip esp32c5 --port <port> --baud 921600 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x2000 bootloader.bin 0x8000 partitions.bin 0xf000 ota_data_initial.bin 0x20000 firmware.bin
```

Close RH and other serial tools before flashing, then unplug and reconnect the
C5. Do not use RH's STM32 node updater. A C5 on 1.1.0 or later can also update
through FPVGate's C5 Receiver Firmware page over UART. An older partition layout
needs the full USB flash. No recalibration is needed after 1.2.2-rc.1.

For direct USB timing, leave the S3 disconnected, add the C5 serial port in RH's
Advanced Settings and restart RH. See the
[USB setup guide](https://github.com/LouisHitchcock/FPVGateC5MK/blob/v1.2.2-rc.3/docs/USB_ROTORHAZARD.md).

## Validation and limitations

- The original parser fails the partial-input regression; the fixed parser passes.
- Portable suites pass: 60 meter/framing checks, 1201 node/UART checks, 48 host
  checks, 305 RH protocol checks and nine USB parser scenarios.
- Five integration tests using stock RH 4.5.0-beta.1's serial client pass.
- Six Linux PTY tests pass with real pySerial, termios, RH delays and retries:
  clean discovery, partial input, overflow, tty echo, fragmented replies and
  repeated discovery without resetting the device process. The original
  parser fails two discovery attempts and recovers after a newline.
- Both board targets build and pass the SRAM guard.

The Linux tests simulate device transport and radio; they do not emulate Pi
hardware, USB enumeration, CDC control lines or the cdc_acm driver. The full RH
web server is not launched. This candidate has not been flashed and tested on
the affected user's Pi or validated at a full race event.
