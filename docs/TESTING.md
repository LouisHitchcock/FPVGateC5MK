# Testing and validation

## PC tests (no hardware)

From `multipilot`, in a POSIX shell (Git Bash on Windows) with `g++`:

```text
bash test/run.sh
```

- `test_mp.cpp`: the meter (tone level, mirror, neighbours, noise), the
  filter, USB framing, and **`power()` = `measure()` exactly** for tones,
  noise, DC and clipping at n=256 and 2048, at +-10 MHz (periodic path) and
  7 MHz (general path).
- `test_node.cpp`: the link protocol on the C5 side: line framing and
  checksums, F/G/Q, range checks, F-then-G ordering, the 1 kHz rate,
  stale-sample dropping, failures to ERR_RF, and scan mode: `P` parsing
  (slots off, bad lists rejected whole), record contents (CRC, mask, values,
  `dt`, sequence), slot order, a waiting command skipping a cycle, G in scan
  mode, failed captures flagged, F leaving scan mode.
- `test_host.cpp`: the reference host library (`host/c5host`) connected to
  the C5's protocol code through a simulated UART: start-up, readings and
  values per slot, timestamps and clock mapping, a corrupted record, a C5
  reboot, a lost slot list, gain and slot changes, and gate-pass detection.

The USB endpoint has its own `test_rh_usb.cpp` checks for API 36 discovery, seats turned off,
checksums, seat selection, frequency/threshold isolation, peak-plateau timing,
split snapshot reads, counter/time rollover, stale samples and partial writes.
`test_link.cpp` runs the actual USB line parser against a simulated driver and
the RH endpoint. It checks revision discovery after partial or overflowed
console input, ordinary console commands, and rejected USB takeover while UART
owns the radio. It does not reproduce Linux port probing or tty echo itself.
`test_rh_usb.py` exercises RH's actual serial client against that endpoint;
see [USB_ROTORHAZARD.md](USB_ROTORHAZARD.md) for its environment and command.

USB and UART ownership switching still needs a physical board test. Passing
the portable tests does not establish RF performance or USB driver behaviour.

## Linux serial integration (no Pi or C5 required)

On Linux, with `g++`, Python 3, `pyserial` and `gevent` installed:

```text
export RH_SOURCE=/path/to/RotorHazard-4.5.0-beta.1
cd multipilot
bash test/run_linux_usb.sh
```

This compiles the production USB line parser and RH protocol endpoint behind a
Linux PTY driver. Stock RH discovery uses real pySerial reads, Linux termios,
timeouts and retries. Tests cover a clean start, unterminated probe input,
overflowed input, actual tty echo, fragmented replies, and repeated discovery
with the device process left running. The original parser guard is compiled
as a control: discovery must fail twice after partial input, then recover after
a newline. The fixed parser must discover all eight API 36 seats and read back
their independent frequencies.

Only the RH discovery/client layer is restarted; the full web server is not
launched. This is Linux software-path validation, not Raspberry Pi emulation.
The radio and node.cpp ownership glue are simulated; PTYs do not model physical
USB, CDC control lines, USB enumeration, the cdc_acm driver or Pi-specific timing.

Validation on 2026-10-18: all six tests passed in 61.063 s under Docker/WSL2
Linux x86_64, Python 3.12.3 and stock RotorHazard 4.5.0-beta.1. The original
parser logged "Unable to fetch revision code" on both discovery attempts and
recovered after a newline; the fixed parser passed all tested input conditions
and three consecutive discovery sessions without a device reset. This confirms
the failure mechanism, but does not identify what sent bytes on the reported Pi.

## Raspberry Pi discovery

On 2026-10-18 a Raspberry Pi 4 running Raspberry Pi OS Lite (Bookworm, 64-bit)
and stock RotorHazard 4.4.0 was connected to a Waveshare C5-Zero over USB.

With 1.2.2-rc.3, RH's discovery failed five times and then accepted one node
with API level 254. Opening the port the way RH does (DTR and RTS cleared, then
opened) printed the bootloader banner every time: the C5 resets on each open.
Repeating RH's sequence (open, 2 s without reading, clear input, send `0x22`,
three reads) returned scanner records on all three reads in each of three runs;
opening without the reset returned `25 24 49` at once. The bytes came from the
C5's own scanner output, held by the host's USB serial driver while RH waited.
ModemManager was running but did not open the port.

With 1.2.2 (tested as 1.2.2-rc.4, then the release image) the same sequence returned `25 24 49` at the first read in five
of five runs, only 428 bytes (the ROM's boot lines) arrived before the host
spoke, and RH 4.4.0 found eight seats at API 36 at the first attempt in three
of three server starts with no read or write errors. `test_link.cpp` checks that
nothing is sent before the host speaks and that queued output is dropped when RH
takes USB.

## Firmware builds

From `multipilot`:

```text
pio run -e c5zero -e xiaoc5
```

Both must build, and `check_sram` must report every symbol `ok`.

The ESP32-S3 host example, from `host/examples/esp32s3`:

```text
pio run
```

## On the bench (C5 only)

`tools/mp.py --port <port>`:

- `cmd "<console command>" --wait S`: see SCAN_ENGINE.md for commands.
- `record --secs N`: per-pilot rate, gaps and dB statistics from the USB
  scanner or the node view.
- `capture MHz --n 4096 [--tone MHz]`: raw I/Q capture and spectrum
  analysis.
- `console`: interactive.

After any change to the radio code, or a new chip revision, board or
ESP-IDF version, run the checks in RADIO_INTERNALS.md section 10.

Bench VTXs generally can't stay on for long: run transmitter tests as short
bursts (30-60 s), and confirm which channel the VTX is really on with a quick
scan first.

## With a host attached

From the FPVGate repository (it only listens to `/events`):

```text
python tools/c5_timing_validation.py --host <fpvgate-ip> --seconds 60 --output run.json
python tools/c5_timing_validation.py --compare old.json run.json
```

Key fields: `perPilotSampleRateHz` (~1030 for 8 pilots), `scanMode`,
`sequenceGaps`, `queueDrops`, `badRecords` (all 0 when healthy),
`onlineFraction` and `readyFraction` (1.0).

For your own host, see HOST_IMPLEMENTATION.md section 9.

Console commands use the radio. With a host attached, send `node off` first
and `node on` afterwards.

## Checklist after a change

1. `bash test/run.sh` passes.
2. Both C5 images build; the host example builds if `host/` changed.
3. On hardware: 60 s with 8 pilots, ~1030 Hz per pilot, 0 gaps, 0 drops,
   0 bad records.
4. `nodeprof` on the C5 if timing changed.
5. RF changes: a VTX burst with `hopprof`, `bandshape` and `hopspikes`.
