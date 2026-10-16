# C5MK 1.2.2-rc.1: direct USB to RotorHazard, fixed and quieter

A release candidate. Every change is in direct USB timing with RotorHazard;
the UART link to FPVGate is unchanged from 1.2.1. Thanks to SanderPuh and
Hazard for the reports behind them.

## What's changed

- **RotorHazard keeps its USB session.** 1.2.1 handed USB back to the console
  after 5 s without a command, but RotorHazard only starts polling once a
  browser connects, so a quiet start lost the node: "Unable to fetch revision
  code" and a flood of WRITE_CURNODE_INDEX errors. The session now lasts until
  a console command line (`status`, `scan`, `node`, ...) asks for USB back.
- **Node API 36 and seats turned off.** RotorHazard 4.5 no longer shows its
  "update your node firmware" notice, its frequency check passes, and a seat
  it turns off (frequency 1111) really stops being scanned.
- **Peaks on the live graph at their true height.** The RSSI RotorHazard shows
  is the highest sample of about the last 250 ms, and its history and Marshal
  page get the peak or nadir since each poll at its own sample time. Before,
  a short pass could fall between RotorHazard's reads and never appear.
- **No blips after a frequency change.** No crossing starts for 100 ms after
  any frequency change, while the scan's filters restart.
- **VTX start-up and channel-change bursts are ignored.** A VTX powering on
  or changing channel sweeps through other channels for 10-200 ms, and each
  of those could count as a crossing. A jump straight off the noise floor is
  now hidden from RotorHazard for 90 ms (RSSI, history, NodePeak and
  crossing) and dropped if it falls back sooner; anything that lasts counts
  as usual, timed from its peak. A quad flying the course never sits on the
  noise floor, so passes are not affected. `node reject` on the console tunes
  it; `node reject 0` turns it off.
- **Fast passes show as crossings.** RotorHazard reads the crossing flag
  about every 100 ms and a fast pass can be over EnterAt for less, so the
  flag now stays set for 250 ms after a pass. Lap counts and times are
  unchanged.

No recalibration is needed after 1.2.1: crossings use the same readings.

## Installing

Choose `c5zero` for Waveshare ESP32-C5-Zero or `xiaoc5` for Seeed XIAO ESP32-C5.
The files below are prefixed with those board names. Each board needs its
matching firmware; the UART pins differ.

A C5 already on 1.1.0 or later can update through FPVGate's C5 Receiver
Firmware page over the UART link: 1.2.2-rc.1 keeps the 1.1.0 partition layout.

A C5 on 1.0 or earlier needs the full USB flash because 1.1.0 changed the
partition layout. Rename the downloaded board-prefixed files to the names
below, or use the unprefixed files in `release/<board>/` at the release tag:

```text
esptool --chip esp32c5 --port <port> --baud 921600 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x2000 bootloader.bin 0x8000 partitions.bin 0xf000 ota_data_initial.bin 0x20000 firmware.bin
```

Close any RH or browser serial connection before a USB flash. Do not use RH's
STM32 node updater on the C5.

[Flashing instructions](https://github.com/LouisHitchcock/FPVGateC5MK/blob/v1.2.2-rc.1/release/README.md)
and [USB setup for RotorHazard](https://github.com/LouisHitchcock/FPVGateC5MK/blob/v1.2.2-rc.1/docs/USB_ROTORHAZARD.md).

## Validation and limitations

- Meter, UART/node, reference-host and USB protocol suites passed: 60, 1201,
  48 and 305 checks, plus 5 tests driving RotorHazard 4.5.0-beta.1's own
  serial-node client.
- On 1.2.2-beta.3 (this release less the burst rejection and crossing
  flag), a Waveshare C5-Zero on USB with stock RotorHazard 4.5.0-beta.1 ran
  9.5 hours overnight without a stall or reboot: 17 RotorHazard restarts after
  10-85 s idle all found the node, 135 channel changes and 51 seat off/on
  cycles were all accepted, and 95,000 malformed frames did no harm.
- On the bench with a VTX: a parked VTX on any Raceband channel left the
  other seven at the noise floor; the power-on sweep and most stepping
  bursts no longer count; no pass lost in 10 swipes.
- A XIAO ESP32-C5 behind a XIAO S3 running FPVGate held 1035-1040 readings
  per second per pilot for 7.5 hours on 1.2.2-beta.3, and updated to
  1.2.2-beta.5 over the link.
- Not yet tested: the XIAO ESP32-C5 over USB, and a full race event.
- A VTX that overshoots by one channel for longer than 90 ms when changing
  channel can still count once on the channel above. `node reject 30 250`
  holds jumps for 250 ms instead, for anyone who wants to try it.
- Once, after RotorHazard was killed in the middle of its USB session, the
  C5 stopped answering until it was reset; it did not happen again. Unplug
  and replug the C5 if RotorHazard reports no receiver nodes.
- In a flight test over USB, EnterAt and ExitAt needed a much wider gap than
  on an RX5808 node: the C5 samples each seat about 1000 times a second, so
  fast fades cross a narrow gap many times. Leave at least 20-30 between them.
