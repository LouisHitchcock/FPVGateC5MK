# Direct USB to RotorHazard

Firmware `1.2.0` and later exposes eight native RotorHazard serial
nodes over the C5's USB port. It needs no S3, Wi-Fi connection, FPVGate plugin
or split-timer/Sensor Tuning PR. Software tests pass and native USB discovery
has been verified on a Waveshare C5-Zero, with timing reported working in
local RH testing. XIAO USB, multi-pilot RF detection and timing accuracy
against video still need validation.

## Setup

1. Download the firmware for your board from the release, or build from
   `multipilot` with `pio run -e c5zero` or `pio run -e xiaoc5`. Follow
   [release/README.md](../release/README.md) to flash it. The same image supports
   both direct USB and the existing S3/UART connection; no mode-specific flash
   is needed.
2. Connect the C5's native USB port directly to the computer running RH, using
   a data cable. Fit the correct antenna. An S3 must not also control the receiver.
3. In RH, open **Advanced Settings > Hardware > Serial Ports**. Add its port,
   such as `COM5` on Windows or `/dev/ttyACM0` on Linux, and restart RH. A Linux
   `/dev/serial/by-id/` path is preferable when available because port numbers
   can change. The host account needs permission to open the port.
4. RH discovers eight nodes and applies its frequency profile. For Raceband,
   select R1-R8, assign each pilot to the matching seat, and calibrate EnterAt
   and ExitAt in the normal Sensor Tuning panel.
5. Start a race in RH. C5MK detects crossings on the device; RH records laps,
   displays them, announces times and saves results.

This is native serial compatibility, not OS-wide automatic port registration.
The port must be configured once. After unplugging, reconnect the board and
restart RH if its serial connection does not recover. RH's discovery and
reconnection behaviour is unchanged.

Close the C5 web dashboard, `mp.py`, flashing tools and serial monitors first:
only one application can own the port. If the FPVGate plugin is also installed,
do not configure a network gate as additional timing hardware unless you
actually want its extra race seats. The direct C5 does not use the plugin.

## Firmware behaviour

- Boot still opens the existing USB scanner. RH's revision request switches
  USB to its standard binary serial protocol, with eight independent seats. From
  1.2.2 it reports node API 36, RH's newest, so RH no longer shows "Node firmware
  update is available"; 1.2.1 and earlier report 35.
- Discovery starts at R1-R8; RH then applies its saved profile. Turning a seat
  off in RH (RH writes 1111, or 0) stops the C5 scanning it; 1.2.1 and earlier
  kept scanning it. Supported frequencies remain 5180-5917 MHz. After each
  frequency write RH runs API 36's register test, which passes when the seat is
  on a supported frequency.
- RSSI is the existing C5 node-filter output divided by four, clamped to RH's
  valid range 1-254. This does not add Widscan58's 255 ms median filter. Calibrate
  this mode rather than assuming thresholds from a different receiver transfer.
- EnterAt/ExitAt hysteresis and peak-plateau timing run on every successful
  capture. Pass timestamps use the plateau midpoint on the C5 clock, reported
  as milliseconds since the pass. USB polling delay is not added to that age;
  RH applies its own round-trip correction.
- Pass counters wrap at 256, as in RH's node protocol. Only the latest pass per
  seat is retained between host reads; this is not an unbounded event queue.
- RH polls each receiver a few times a second; the C5 samples it about 1000
  times a second. Each poll returns the latest RSSI, and for RH's RSSI history
  (the Sensor Tuning graph and Marshal) one extremum of the samples since the
  previous poll, at its own sample times: the peak if it rose further from the
  extremum reported last than the nadir fell, otherwise the nadir. A short pass
  between polls therefore appears in the graph at its real height. The full
  1 kHz trace is not transferred.
- Invalid checksums, unsupported frequencies and invalid seat indexes do not
  change settings. Thresholds are 1-254; detection is suspended while EnterAt
  is not higher than ExitAt. Failed captures do not produce timing samples.
- Crossings are cancelled after a sample gap over 100 ms. Stale reads report
  RSSI 1 with no crossing rather than showing the last reading as live.
- USB does not take the radio from an already active S3/UART session. While RH
  owns it, UART commands are ignored and framed scanner output is suppressed.
  Five seconds without a valid RH request releases USB ownership. `scan on`
  resumes the dashboard scanner afterward. Power-cycle before switching an
  actively S3-controlled receiver to standalone USB.
- Thresholds are held in RAM; RH supplies calibration from its saved profile.
  C5 receiver gain remains the existing stored node gain. USB firmware updating
  through RH's STM32 updater is not advertised; use the normal C5 flash tool.

## Checks

Run the portable C++ tests with `bash test/run.sh` from `multipilot`.
For integration tests, install RH's dependencies, set `RH_SOURCE` to a standard
RH checkout (4.5.0-beta.1 was tested), and run:

```text
python -m unittest discover -s test -p test_rh_usb.py
```

The integration test feeds RH's actual `serial_node.discover()` and
`RHInterface` through a simulated serial port backed by the compiled C++
endpoint. It checks eight-seat discovery, firmware identification, independent
tuning, disabling a seat, calibration capture and peak-timestamp lap recording.
No live RH server, serial port or gate is contacted.

Local validation on 2026-10-13: 160 USB protocol checks, four integration tests
against stock RH 4.5.0-beta.1, 60 meter checks, 1201 node/UART checks and 48
reference-host checks passed. Both `c5zero` and `xiaoc5` development and 1.2.0
release firmware built successfully; their SRAM guard checks passed.

The Waveshare C5-Zero was subsequently flashed with all written
sections hash-verified. Boot checks reported live captures, and stock RH's
actual serial client discovered API 35 firmware `1.2.0-dev.1`, eight slots,
R1-R8 frequencies and live RSSI 28-31. A local RH server was then switched to
this USB receiver, preserving the R6 pilot assignment and two-minute heat.
The tester reported it working. This does not establish RF rate, gate-pass
accuracy against video, USB reconnection or XIAO behaviour.

Before deploying, use a spare C5 to check discovery, all eight graphs,
frequency/threshold readback, deliberate gate passes, unplug/replug and
switching back to the original S3/UART connection. Compare recorded pass times
against video before making accuracy claims.

## Reference

[Widscan58's RH setup](https://github.com/CapnBry/widscan58/blob/main/docs/RotorHazard.md)
demonstrates this native serial-node approach. C5MK's endpoint is independently
implemented from RotorHazard's public serial-client command IDs, checksums,
byte order and API 35 data layouts; no Widscan58 firmware code was copied.
