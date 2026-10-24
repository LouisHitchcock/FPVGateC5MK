# C5MK 1.2.4-rc.1: RotorHazard USB timing fixes

24 October 2026. Release candidate for comparison against RX5808 timing
hardware and an external camera reference. Stable remains 1.2.3.

## Changes since 1.2.3

- Native USB live/Marshal alignment: a history poll containing a crossing's
  peak now preserves that peak instead of sometimes reporting a multipath
  dip. Live passes and recalculation can therefore use the same peak samples.
  Against RH 4.4.0's serial client and a port of its Marshal recalculation,
  the tested ten-pass traces, five random seeds and 80-250 ms poll intervals
  agree within 1 ms. This is software consistency, not measured RF accuracy.
- Multipath crossing merge: after falling below ExitAt, wait 300 ms before
  completing a pass; re-entering above EnterAt during that window continues
  the same pass. The recorded timestamp remains the peak-plateau midpoint,
  not the later notification time. This changes detection behavior from
  1.2.3 and is not identical to an RX5808 node. Additional smoothing is off.
- Repeated-frequency seats: one capture, filtered value and sample time
  are shared by seats tuned to the same frequency, in USB and UART modes.
  Their thresholds and crossing counters remain independent. This avoids
  different scan samples when several seats compare the same transmitter.
- Includes the database crossing-comparison/replay tool
  `multipilot/tools/rh_db_report.py`; it does not change production timing.

USB still uses RH's native serial-node interface, with no FPVGate plugin or
S3 needed. The FPVGate Alpha 5/plugin clock-mapping fix is a separate change
for the UART/S3 path; USB uses RH's own timing interface instead.
RF power estimation and the production peak-midpoint estimator are unchanged.
The partition layout, UART pins and existing Linux/Pi discovery fixes remain.

## Flash and setup

In [the C5 flasher](https://fpvgate.xyz/c5.html#flasher), choose your board,
then **Multi Pilot RC · UART / USB · 1.2.4-rc.1**. Stable 1.2.3 remains a
separate choice. Use `c5zero` for Waveshare ESP32-C5-Zero or `xiaoc5` for
Seeed Studio XIAO ESP32-C5; their UART pins differ. Full USB flashing writes
all four images, including the OTA selector for the new application.

Close RH, serial monitors and browser USB connections before flashing.
Power-cycle afterwards. For direct RH USB, disconnect any S3 controller,
add the C5's native serial port in RH's hardware settings and restart RH.
Do not use RH's STM32 firmware updater. No S3 Alpha 5 or plugin upgrade is
required for the direct-USB test.

For command-line flashing, use the four files from `release/<board>/` at
this tag, or rename the matching board-prefixed download assets:

```text
esptool --chip esp32c5 --port <port> --baud 921600 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x2000 bootloader.bin 0x8000 partitions.bin 0xf000 ota_data_initial.bin 0x20000 firmware.bin
```

Existing 1.1.0+ UART receivers can also install the board's `firmware.bin`
through FPVGate's C5 Receiver Firmware page. A receiver on 1.0 or earlier
needs the complete USB flash. Application-only updates do not replace an
old bootloader; use a complete flash when upgrading from before 1.2.2 for
Linux/Pi USB discovery.

## Priority test

1. Preserve the original RH database, receiver versions, frequencies,
   thresholds and node/pilot assignments. Calibrate the C5 independently of
   the RX5808; equal numeric thresholds do not mean equal RF levels.
2. Prioritize C5 direct USB and RX5808 observing the same physical passes,
   with the camera as an independent reference. Check single-pilot timing
   first, then multiple transmitters and realistic multipath.
3. Keep individual race-relative crossing timestamps, including the first
   crossing, and record live versus recalculated results separately. Compare
   signed offset, scatter, worst deviation and missed/extra crossings, not
   only lap durations. A 120 fps camera has an 8.33 ms frame interval and
   alone cannot establish sub-millisecond RF accuracy.
4. Repeat through C5 UART -> FPVGate Alpha 5 -> RH plugin 2.3.2-rc.1 to
   measure any additional difference in that path. Do not allow USB RH and
   an S3 to control the same C5 simultaneously.

The default 300 ms merge can combine distinct rapid revisits, and crossing
rules differ from RX5808. Preserve the defaults for the first run and report
any changes. Console `node detect 0` disables merging, but settings are
temporary and the console takes USB ownership away from RH; do not issue
console commands during a timing run.

## Validation and limits

Portable tests: 60 meter/framing checks, 1256 node checks, 120 host checks,
382 USB endpoint checks, eleven USB parser scenarios and 37 browser lap
checks pass. Six integration tests pass against each of RH 4.4.0 and
4.5.0-beta.1's real serial clients, including the live/Marshal comparison above.

Both board images are built with pinned ESP-IDF 5.5.1 and checked for SRAM
placement, ESP32-C5 identity, embedded board/version, partition layout and
application fit. Release downloads include SHA-256 checksums.

These release-candidate binaries have not been flashed or race-tested on
physical hardware as part of packaging. Camera accuracy, physical XIAO USB,
multi-transmitter behavior and USB reconnect validation remain pending.
Earlier hardware results are not qualification of these images. Do not
treat live/Marshal agreement as proof of accuracy against the physical line.
