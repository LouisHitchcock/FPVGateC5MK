# C5MK V1.2.2-RC.4: RotorHazard USB discovery on Linux and Raspberry Pi

Release candidate for Waveshare ESP32-C5-Zero and Seeed XIAO ESP32-C5.
Fixes RotorHazard failing to find the C5 over USB on Linux, including the
Raspberry Pi. Includes everything from 1.2.2-rc.3 and 1.2.2-rc.1. The UART link
and 1.1.0 partition layout are unchanged.

## USB discovery fix

On Linux, RotorHazard could report "Unable to fetch revision code for serial
node" for a C5 on 1.2.2-rc.3 or earlier, or find one node with API level 254
instead of eight seats.

Opening the C5's USB port on Linux resets the C5. RH then waits 2 s without
reading before asking for the revision code. The scanner output the C5 sent in
that time was held by the computer's USB serial driver and reached RH ahead of
the reply, so RH read scanner data as its revision code. Each retry reopened
the port, which reset the C5 again. Windows does not reset the C5 when the port
opens, which is why discovery worked there.

From this release:

- After a reset the C5 sends nothing on USB until the computer sends something.
  RH's first byte is its revision request. The C5 dashboard and `mp.py` send a
  command when they connect, and `mp.py console` shows nothing until a line is
  typed.
- Scanner output still waiting to be sent when RH takes the USB port is dropped,
  so RH's first bytes back are its replies.
- The bootloader logs only warnings, so at reset only a few ROM lines reach the
  USB port.

## Included from 1.2.2-rc.3 and 1.2.2-rc.1

- An unfinished or overflowed USB console line no longer blocks RH's revision
  request.
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
through FPVGate's C5 Receiver Firmware page over UART; that updates the firmware
but not the bootloader, which only affects USB use with RH, so use the USB flash
for a C5 used directly with RH on Linux. An older partition layout needs the
full USB flash. No recalibration is needed after 1.2.2-rc.1.

For direct USB timing, leave the S3 disconnected, add the C5 serial port in RH's
Advanced Settings and restart RH. See the
[USB setup guide](https://github.com/LouisHitchcock/FPVGateC5MK/blob/v1.2.2-rc.4/docs/USB_ROTORHAZARD.md).

## Validation and limitations

- Reproduced on a Raspberry Pi 4 (Raspberry Pi OS Bookworm, 64-bit) with stock
  RotorHazard 4.4.0 and a Waveshare C5-Zero. With 1.2.2-rc.3, discovery failed
  five times and then found one node with API level 254. With 1.2.2-rc.4, RH
  found eight seats at API 36 at the first attempt in five of five server
  starts (two of them with this release image), with no read or write errors.
- Portable suites pass on Windows and on the Pi: 60 meter/framing checks, 1201
  node/UART checks, 48 host checks, 305 RH protocol checks and eleven USB parser
  scenarios. The two new scenarios fail against the 1.2.2-rc.3 parser.
- Five integration tests using stock RH 4.4.0's serial client pass, and six
  Linux PTY discovery tests pass on the Pi.
- Both board targets build and pass the SRAM guard.

The Seeed XIAO ESP32-C5 image builds and passes the tests but was not run on
hardware with RH. Discovery and polling were tested; a race with laps through
RH was not. Not yet validated at a full race event.
