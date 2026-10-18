# C5MK V1.2.2: direct USB to RotorHazard, Raspberry Pi included

Every change is in direct USB timing with RotorHazard; the UART link to
FPVGate is unchanged from 1.2.1. Direct USB now works on Linux and the
Raspberry Pi, where RotorHazard could not find the C5. Thanks to SanderPuh,
Hazard and pigodfpv for the reports behind these fixes.

## What's changed

- **RotorHazard finds the C5 on Linux and the Raspberry Pi.** On Linux,
  opening the C5's USB port resets the C5, and RotorHazard then waits 2 s
  without reading before asking for the revision code. The scanner output the
  C5 sent in that time was held by the computer's USB serial driver and
  reached RotorHazard ahead of the reply, so it reported "Unable to fetch
  revision code for serial node" or found one node with API level 254. Each
  retry reopened the port and reset the C5 again. After a reset the C5 now
  sends nothing on USB until the computer sends something, scanner output
  still queued when RotorHazard takes the port is dropped, and the bootloader
  logs only warnings. Windows does not reset the C5 when the port opens,
  which is why discovery worked there.
- **Stray serial input no longer blocks discovery.** An unfinished or
  overflowed console line could stop the C5 recognising RotorHazard's revision
  request, even across RotorHazard restarts.
- **RotorHazard keeps its USB session.** 1.2.1 handed USB back to the console
  after 5 s without a command, but RotorHazard only starts polling once a
  browser connects, so a quiet start lost the node. The session now lasts until
  a console command line (`status`, `scan`, `node`, ...) asks for USB back.
- **Node API 36 and seats turned off.** RotorHazard 4.5 no longer shows its
  "update your node firmware" notice, its frequency check passes, and a seat
  it turns off (frequency 1111) really stops being scanned.
- **Peaks on the live graph at their true height.** The RSSI RotorHazard shows
  is the highest sample of about the last 250 ms, and its history and Marshal
  page get the peak or nadir since each poll at its own sample time.
- **No blips after a frequency change.** No crossing starts for 100 ms after
  any frequency change, while the scan's filters restart.
- **VTX start-up and channel-change bursts are ignored.** A jump straight off
  the noise floor is hidden from RotorHazard for 90 ms and dropped if it falls
  back sooner; anything that lasts counts as usual, timed from its peak. `node
  reject` on the console tunes it; `node reject 0` turns it off.
- **Fast passes show as crossings.** The crossing flag RotorHazard reads stays
  set for 250 ms after a pass. Lap counts and times are unchanged.

The C5 dashboard and `mp.py` send a command when they connect, so they are
unaffected by the C5 staying quiet after a reset; `mp.py console` shows nothing
until a line is typed.

No recalibration is needed after 1.2.1: crossings use the same readings.

## Installing

Choose `c5zero` for Waveshare ESP32-C5-Zero or `xiaoc5` for Seeed XIAO ESP32-C5.
The files below are prefixed with those board names. Each board needs its
matching firmware; the UART pins differ. Or use
[the web flasher](https://fpvgate.xyz/c5.html).

A C5 already on 1.1.0 or later can update through FPVGate's C5 Receiver
Firmware page over the UART link: 1.2.2 keeps the 1.1.0 partition layout. That
updates the firmware but not the bootloader, so a C5 used directly with
RotorHazard on Linux should get the USB flash below.

A C5 on 1.0 or earlier needs the full USB flash because 1.1.0 changed the
partition layout. Rename the downloaded board-prefixed files to the names
below, or use the unprefixed files in `release/<board>/` at the release tag:

```text
esptool --chip esp32c5 --port <port> --baud 921600 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x2000 bootloader.bin 0x8000 partitions.bin 0xf000 ota_data_initial.bin 0x20000 firmware.bin
```

Close any RotorHazard or browser serial connection before a USB flash, then
unplug and reconnect the C5. Do not use RotorHazard's STM32 node updater on the
C5.

[Flashing instructions](https://github.com/LouisHitchcock/FPVGateC5MK/blob/v1.2.2/release/README.md)
and [USB setup for RotorHazard](https://github.com/LouisHitchcock/FPVGateC5MK/blob/v1.2.2/docs/USB_ROTORHAZARD.md).

## Validation and limitations

- On a Raspberry Pi 4 (Raspberry Pi OS Bookworm, 64-bit) with stock
  RotorHazard 4.4.0 and a Waveshare C5-Zero, 1.2.2-rc.3 failed discovery five
  times and then found one node with API level 254. This fix found eight seats
  at API 36 at the first attempt in all seven server starts (as 1.2.2-rc.4 and
  as this release image), with no read or write errors.
- Meter, UART/node, reference-host and USB protocol suites pass on Windows and
  on the Pi: 60, 1201, 48 and 305 checks, plus eleven USB parser scenarios.
  Five tests driving stock RotorHazard 4.4.0's serial-node client and six Linux
  PTY discovery tests pass on the Pi.
- On 1.2.2-beta.3, a Waveshare C5-Zero on USB with stock RotorHazard
  4.5.0-beta.1 ran 9.5 hours overnight without a stall or reboot.
- A XIAO ESP32-C5 behind a XIAO S3 running FPVGate held 1035-1040 readings per
  second per pilot for 7.5 hours on 1.2.2-beta.3, and updated through the gate
  to 1.2.2-rc.3.
- Both board targets build and pass the SRAM guard.
- Not yet tested: the XIAO ESP32-C5 over USB with RotorHazard, a race with laps
  through RotorHazard on the Pi, and a full race event.
- A VTX that overshoots by one channel for longer than 90 ms when changing
  channel can still count once on the channel above. `node reject 30 250`
  holds jumps for 250 ms instead.
- In a flight test over USB, EnterAt and ExitAt needed a much wider gap than on
  an RX5808 node: the C5 samples each seat about 1000 times a second, so fast
  fades cross a narrow gap many times. Leave at least 20-30 between them.
