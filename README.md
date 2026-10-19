# FPVGate C5MK

Multi-pilot 5.8 GHz RSSI receiver firmware for the ESP32-C5.

C5MK turns a single ESP32-C5 module into an RF front end that measures up to
eight FPV video transmitters about 1000 times a second each. It uses the C5's
own 5 GHz Wi-Fi radio as a receiver: no RX5808 modules, no SPI mods.

The same firmware works with three kinds of lap timer:

- **[RotorHazard](https://github.com/RotorHazard/RotorHazard)**, over USB. The
  C5 plugs straight into the computer running RH and appears as eight native
  RH nodes: no S3, network connection or plugin.
- **[FPVGate](https://github.com/LouisHitchcock/FPVGate)**, over UART, as the
  receiver node of its multi-pilot mode.
- **Your own timer**, over the same UART link. The protocol is simple and
  `host/` has a ready-made library.

The host gives the C5 a list of up to eight frequencies. The C5 then cycles
through them on its own and measures every pilot. Over UART it streams
timestamped RSSI for the host to calibrate, display and detect laps from.
Over USB to RotorHazard it also detects the gate crossings itself and
reports them to RH.

## What it does

- **8 pilots at ~1030 readings per pilot per second**, continuous, measured
  over 60 s runs with no gaps or drops (8 Raceband pilots).
- **5180-5917 MHz**: Raceband R1-R8 and most other 5.8 GHz bands. Boscam E7
  and E8 (5925, 5945) are out of range.
- **Band-power RSSI** from raw I/Q captures: the power in a ~10 MHz band
  around each pilot, so the reading follows the whole FM signal rather than
  one slice of it.
- **Per-reading timestamps** from the C5's clock, so lap times don't depend on
  USB, UART or host loop latency.
- **Native RotorHazard nodes** (from `1.2.0`):
  - eight seats on one USB port, speaking RH's standard serial node protocol
    (node API 36)
  - EnterAt/ExitAt crossing detection on the C5, timed at the middle of the
    peak
  - RH's Sensor Tuning, frequency profiles and race management work as they
    do with RX5808 nodes
- **Receive only.** The firmware never enables the transmitter, the DAC or
  tone generation.

## Status

| | |
|---|---|
| Direct USB to RotorHazard (no S3), from `1.2.0` | Waveshare C5-Zero: eight R1-R8 nodes and live RSSI verified, timing reported working in local RH testing; XIAO USB validation pending |
| Single-quad races through FPVGate | Working |
| 2-8 pilot races (RotorHazard or FPVGate) | Implemented; not yet raced with two or more real pilots |
| Waveshare ESP32-C5-Zero (`c5zero`) | Tested |
| Seeed Studio XIAO ESP32-C5 (`xiaoc5`) | Tested: 1031 Hz per pilot through FPVGate, VTX on R8 received |
| Lap accuracy against video | Not measured yet |

Known limitations are listed in [docs/RF_RESEARCH.md](docs/RF_RESEARCH.md)
(image rejection, post-hop spikes, untested bands).

## Quick start

You need an ESP32-C5 board with a 5 GHz antenna on it. Flash the ready-built
firmware for your board from `release/<board>/` (see
[release/README.md](release/README.md)), or build it with
[PlatformIO](https://platformio.org/install/cli):

```text
cd multipilot
pio run -e c5zero -t upload --upload-port <port>
```

On Windows run PlatformIO from PowerShell or cmd, not Git Bash. Then check
the radio came up, over the C5's USB port:

```text
pip install pyserial
python tools/mp.py --port <port> cmd "status"
```

The status shows `wifi=1`, a `gainmax` (89 on chip v1.0) and the boot checks,
the last of which should end in `-> live`. For a live view of every channel,
open **[c5mk.fpvgate.xyz](https://c5mk.fpvgate.xyz)** in a browser with Web
Serial (Chrome, Edge, Opera or a recent Firefox) and
press Connect: it runs in the browser over Web Serial, nothing is uploaded.
The same page is `multipilot/web/index.html`, which also works opened from
disk.

Every image supports all three connections below; there's no separate
USB-only or UART-only firmware.

### With RotorHazard (USB)

1. Connect the C5's USB port directly to the computer running RotorHazard,
   using a data cable.
2. In RH, open **Advanced Settings > Hardware > Serial Ports**, add the C5's
   port (`COM5` on Windows, `/dev/ttyACM0` or a `/dev/serial/by-id/` path on
   Linux) and restart RH.
3. RH discovers eight nodes and applies its frequency profile. Set the
   frequencies, assign pilots to seats, and calibrate EnterAt and ExitAt in
   the normal Sensor Tuning panel. Readings differ from an RX5808's, so
   calibrate rather than reusing old thresholds.
4. Race as usual: the C5 detects the crossings; RH records the laps, announces
   them and saves the results.

Only one program can own the port, so close the C5 dashboard, `mp.py` and
serial monitors first. The full guide, including how C5MK maps its readings
and crossings onto RH's protocol, is
[docs/USB_ROTORHAZARD.md](docs/USB_ROTORHAZARD.md).

### With FPVGate (UART)

Wire the C5 to FPVGate's ESP32-S3 as below and set **Settings >
Configuration > Receiver Module** to ESP32-C5. FPVGate's
`docs/C5_MULTI_PILOT.md` is the user guide. The link uses the pins the RX5808
used to:

| Host (XIAO ESP32-S3) | `c5zero` | `xiaoc5` |
|---|---|---|
| GPIO4 / D3 (TX) | GPIO4 (RX) | GPIO23 / D4 (RX) |
| GPIO5 / D4 (RX) | GPIO5 (TX) | GPIO24 / D5 (TX) |
| 5V (VUSB) | 5V | 5V (VUSB) |
| GND | GND | GND |

The link is 921600 baud, 8N1, 3.3 V logic, no flow control.

**Power the C5 from 5 V, not from the S3's 3V3 pin.** The C5's radio draws a
burst of current as it starts, and the XIAO S3's 3.3 V regulator, already
running the S3's own Wi-Fi, can't supply it: powered that way, a XIAO
ESP32-C5 never finished booting. 5V to 5V lets the C5 use its own regulator.
See [docs/INTEGRATION_GUIDE.md](docs/INTEGRATION_GUIDE.md) section 1.

### With your own timer

Read [docs/INTEGRATION_GUIDE.md](docs/INTEGRATION_GUIDE.md) for the C5 side
and [docs/HOST_IMPLEMENTATION.md](docs/HOST_IMPLEMENTATION.md) for the
receiving side. `host/` has a portable C++ library and an ESP32-S3 example.
Wiring and power are as for FPVGate above.

## Releases and updates

Each release has ready-built firmware for both boards in
`release/<board>/` ([release/README.md](release/README.md)). Flash a C5 over
USB once with the full set (bootloader, partition table, OTA data and
firmware).

- **RotorHazard and your own timer:** later releases are flashed over USB
  the same way.
- **FPVGate** (1.9.0-Multi-Alpha-3 and later) can update the C5 over the
  existing link from **Settings > System > C5 Receiver Firmware**, with no
  USB cable. It lists the releases, checks the image is for the C5's board,
  and the C5 goes back to its previous firmware if the new one can't reach
  the gate. This needs firmware 1.1.0 or later on the C5; C5s on 1.0 or
  earlier need one USB flash first, because 1.1.0 changed the flash layout.

## Documentation

| Document | Contents |
|---|---|
| [Technical paper (PDF)](docs/FPVGate_C5MK_Paper.pdf) | *A Single-Receiver, Software-Defined RSSI Front End for Multi-Pilot FPV Race Timing at 1 kHz per Pilot*: the full development story, methods, results and limitations. Start here for the overview. |
| [USB_ROTORHAZARD.md](docs/USB_ROTORHAZARD.md) | Racing with RotorHazard over USB: setup, and how the C5 presents eight native RH nodes |
| [INTEGRATION_GUIDE.md](docs/INTEGRATION_GUIDE.md) | Building the C5 into your own system: boards, antenna, wiring, flashing, porting, gain, calibration |
| [HOST_IMPLEMENTATION.md](docs/HOST_IMPLEMENTATION.md) | Writing the receiving side (an ESP32-S3 or any other host): the reference library, the protocol step by step, timestamps, lap detection |
| [LINK_PROTOCOL.md](docs/LINK_PROTOCOL.md) | The UART protocol: text lines, binary scan records, clock mapping |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the receiver and the host share the work |
| [SCAN_ENGINE.md](docs/SCAN_ENGINE.md) | The C5's scan loop, timing budget and USB console |
| [RADIO_INTERNALS.md](docs/RADIO_INTERNALS.md) | How the firmware drives the C5's radio: build settings, I/Q dump engine, tuning, gain, the meter |
| [RF_RESEARCH.md](docs/RF_RESEARCH.md) | Bench measurements: retune time, passband, image rejection, the path to 1 kHz |
| [FPVGATE_INTEGRATION.md](docs/FPVGATE_INTEGRATION.md) | The FPVGate (ESP32-S3) side, as a reference host implementation |
| [TESTING.md](docs/TESTING.md) | PC tests and hardware checks |
| [LICENSING.md](docs/LICENSING.md), [PROVENANCE.md](docs/PROVENANCE.md) | The licence, and where each technical fact came from |

## Repository layout

```text
multipilot/                 the C5 firmware
  src/                      firmware source (ESP-IDF, C++), including the RotorHazard node endpoint (rh_usb.*)
  test/                     PC tests: meter, framing, link protocol, RH node protocol, host library (no hardware)
  tools/mp.py               USB console, recorder and raw-capture analysis
  web/index.html            live dashboard over Web Serial
host/                       the receiving side, for your own timer
  c5host/                   portable C++ library for the UART link and gate passes
  examples/esp32s3/         minimal ESP32-S3 host (PlatformIO, Arduino)
docs/                       documentation
release/<board>/            ready-built firmware for the current release
```

## Acknowledgements

Thank you to [C5VRX](https://github.com/Twotoz/C5VRX) (Twotoz and
contributors) for the initial discovery that the ESP32-C5 can receive analog
FPV video, and for pointing this project in the right direction. Thanks also
to [ESP-SDR](https://github.com/ESPARGOS/esp-sdr) (ESPARGOS), who first
published that ESP32 radios have a raw I/Q capture path.

C5MK contains no code from either project; see
[docs/PROVENANCE.md](docs/PROVENANCE.md).

Thanks to [RaceFPV](https://github.com/RaceFPV) for finding that rewriting
the gain force on every hop disturbs the receiver, the fix behind 1.2.1's
steadier readings, and for pinning the build platform
([#1](https://github.com/LouisHitchcock/FPVGateC5MK/pull/1)).

## Licence

CC BY-NC-SA 4.0 for non-commercial use, the same as FPVGate. Commercial use
(selling hardware or kits running this firmware, or a paid service) needs a
separate commercial licence. See [LICENSE](LICENSE) and
[docs/LICENSING.md](docs/LICENSING.md).
