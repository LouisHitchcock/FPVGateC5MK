# FPVGate C5MK

Multi-pilot 5.8 GHz RSSI receiver firmware for the ESP32-C5.

C5MK turns a single ESP32-C5 module into an RF front end that measures up to
eight FPV video transmitters about 1000 times a second each. It uses the C5's
own 5 GHz Wi-Fi radio as a receiver: no RX5808 modules, no SPI mods. It is
the receiver node of [FPVGate](https://github.com/LouisHitchcock/FPVGate)'s
multi-pilot mode, and its UART protocol is simple enough to drive from any
other lap timer or host.

The host sends a list of up to eight frequencies once. The C5 then cycles
through them on its own and streams timestamped RSSI for every pilot over a
921600-baud UART, ready for calibration, display and lap detection.

## What it does

- **8 pilots at ~1030 readings per pilot per second**, continuous, measured
  over 60 s runs with no gaps or drops (8 Raceband pilots).
- **5180-5917 MHz**: Raceband R1-R8 and most other 5.8 GHz bands. Boscam E7
  and E8 (5925, 5945) are out of range.
- **Band-power RSSI** from raw I/Q captures: the power in a ~10 MHz band
  around each pilot, so the reading follows the whole FM signal rather than
  one slice of it.
- **Per-reading timestamps** from the C5's clock, so lap times don't depend on
  UART or host loop latency.
- **Receive only.** The firmware never enables the transmitter, the DAC or
  tone generation.

## Status

| | |
|---|---|
| Single-quad races through FPVGate | Working |
| 2-8 pilot races through FPVGate | Implemented; not yet raced with two or more real pilots |
| Waveshare ESP32-C5-Zero (`c5zero`) | Tested |
| Seeed Studio XIAO ESP32-C5 (`xiaoc5`) | Builds; not yet tested on hardware |
| Lap accuracy against video | Not measured yet |

Known limitations are listed in [docs/RF_RESEARCH.md](docs/RF_RESEARCH.md)
(image rejection, post-hop spikes, untested bands).

## Quick start

You need an ESP32-C5 board, a 5 GHz antenna on it, and
[PlatformIO](https://platformio.org/install/cli).

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
the last of which should end in `-> live`. Open `web/index.html` in Chrome or
Edge for a live view of all eight Raceband channels over Web Serial.

To use it with FPVGate, wire it as below and set **Settings > Configuration >
Receiver Module** to ESP32-C5 (FPVGate's `docs/C5_MULTI_PILOT.md` is the user
guide). To use it with your own system, read
[docs/INTEGRATION_GUIDE.md](docs/INTEGRATION_GUIDE.md) for the C5 side and
[docs/HOST_IMPLEMENTATION.md](docs/HOST_IMPLEMENTATION.md) for the receiving
side; `host/` has a ready-made library and an ESP32-S3 example.

## Wiring

The C5 needs 3.3 V, ground and two UART lines. On an FPVGate built on a XIAO
ESP32-S3, the link uses the pins the RX5808 used to:

| Host (XIAO ESP32-S3) | `c5zero` | `xiaoc5` |
|---|---|---|
| GPIO4 / D3 (TX) | GPIO4 (RX) | GPIO23 / D4 (RX) |
| GPIO5 / D4 (RX) | GPIO5 (TX) | GPIO24 / D5 (TX) |
| 3V3 | 3V3 | 3V3 |
| GND | GND | GND |

The link is 921600 baud, 8N1, 3.3 V logic, no flow control. The C5's native
USB is for flashing and the development console only.

## Documentation

| Document | Contents |
|---|---|
| [Technical paper (PDF)](docs/FPVGate_C5MK_Paper.pdf) | *A Single-Receiver, Software-Defined RSSI Front End for Multi-Pilot FPV Race Timing at 1 kHz per Pilot*: the full development story, methods, results and limitations. Start here for the overview. |
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
  src/                      firmware source (ESP-IDF, C++)
  test/                     PC tests: meter, framing, link protocol, host library (no hardware)
  tools/mp.py               USB console, recorder and raw-capture analysis
  web/index.html            live dashboard over Web Serial
host/                       the receiving side, for your own timer
  c5host/                   portable C++ library for the UART link and gate passes
  examples/esp32s3/         minimal ESP32-S3 host (PlatformIO, Arduino)
docs/                       documentation
```

## Acknowledgements

Thank you to [C5VRX](https://github.com/Twotoz/C5VRX) (Twotoz and
contributors) for the initial discovery that the ESP32-C5 can receive analog
FPV video, and for pointing this project in the right direction. Thanks also
to [ESP-SDR](https://github.com/ESPARGOS/esp-sdr) (ESPARGOS), who first
published that ESP32 radios have a raw I/Q capture path.

C5MK contains no code from either project; see
[docs/PROVENANCE.md](docs/PROVENANCE.md).

## Licence

CC BY-NC-SA 4.0 for non-commercial use, the same as FPVGate. Commercial use
(selling hardware or kits running this firmware, or a paid service) needs a
separate commercial licence. See [LICENSE](LICENSE) and
[docs/LICENSING.md](docs/LICENSING.md).
