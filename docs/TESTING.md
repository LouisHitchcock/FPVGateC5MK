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

Expected: 60 meter checks, 1139 node checks and 48 host checks, all passing.

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
