# Integration guide

How to build a C5MK receiver into your own lap timer or system. This page
covers the C5 side: hardware, flashing, porting to another board, gain and
calibration. The firmware for the receiving device is in
[HOST_IMPLEMENTATION.md](HOST_IMPLEMENTATION.md).

## 1. Hardware

### The C5 board

Any ESP32-C5 board should work: the firmware only uses the chip's own radio,
one UART and native USB. Two are set up already:

| Board | Environment | Link pins (RX / TX) | Status |
|---|---|---|---|
| Waveshare ESP32-C5-Zero | `c5zero` | GPIO4 / GPIO5 | Tested |
| Seeed Studio XIAO ESP32-C5 | `xiaoc5` | GPIO23 (D4) / GPIO24 (D5) | Builds, not yet tested on hardware |

Other boards need their link pins set (section 3).

### Antenna

Use an antenna made for 5.8 GHz. Some boards have an RF switch that selects
between a PCB antenna and a connector; C5MK doesn't drive any switch GPIO, so
check which antenna your board selects by default. The bench figures in this
repository are from a C5-Zero with its default antenna path.

### Power and wiring

The C5 needs 3.3 V and ground, plus two UART lines to the host:

| C5 | Host |
|---|---|
| link RX | host UART TX |
| link TX | host UART RX |
| 3V3 | 3.3 V supply |
| GND | GND |

The link is 921600 baud, 8N1, 3.3 V logic, no flow control. Keep the wires
short. Leave the C5's strapping pins (2, 3, 7, 8, 9, 25-28) free. The C5's
native USB is only needed for flashing and the development console, and can
be left unconnected in use.

## 2. Flashing

Install [PlatformIO](https://platformio.org/install/cli), then from
`multipilot/`:

```text
pio run -e c5zero -t upload --upload-port <port>
```

On Windows, run PlatformIO from PowerShell or cmd, not Git Bash. The first
build downloads ESP-IDF and takes several minutes. The build fails on purpose
if static data grows into the SRAM the radio's capture engine writes to (see
[RADIO_INTERNALS.md](RADIO_INTERNALS.md)).

Check it over USB:

```text
pip install pyserial
python tools/mp.py --port <port> cmd "status"
```

Look for `wifi=1`, `gainmax=89` (chip v1.0) and a boot check ending in
`-> live`. If the C5 doesn't come back after flashing, unplug and replug it;
`mp.py` opens the port with DTR and RTS low so it doesn't reset the C5.

At boot the C5 runs a USB scanner over Raceband for the dashboard
(`web/index.html`). The first valid command from the host on the link UART
switches it to node mode and hands the radio to the host.

## 3. Porting to another C5 board

The only board-specific setting is the pair of link pins. Add an environment
to `multipilot/platformio.ini`:

```ini
[env:myboard]
extends = env:c5zero
board_build.cmake_extra_args =
    -DNODE_UART_RX_PIN=10
    -DNODE_UART_TX_PIN=11
```

The pins have to go through `cmake_extra_args`: PlatformIO's `build_flags`
don't reach the sources in an ESP-IDF build. Pick pins that aren't strapping
pins, the USB pins (13, 14) or UART0's console pins. After flashing, the
`node` console command shows `uart tx <n> rx <n>`; check it matches.

`sdkconfig.defaults` sets 4 MB of flash, which suits any C5 module with 4 MB
or more.

## 4. Gain

The C5 receives at a fixed gain index, 0 to 89, set by the host (`G`). It
remembers the last value across reboots. There is no automatic gain control:
a fixed gain keeps readings comparable between pilots and over time.

- **30** is the default, a bench compromise: at 36 a quad at the bench
  clipped the ADC occasionally, at 24 it was mostly clean, and at 30 it still
  clipped now and then at very close range.
- Higher gain lifts weak, distant signals but a close quad clips sooner.
  Clipping flattens the top of a pass and makes the peak time less exact.
- Lower gain suits small rooms and close gates.

The right value depends on the gate distance and the VTX power at the event.
Choose it once per venue, then calibrate thresholds at that gain. To check
for clipping, use the USB scanner (`node off`, then `scan on`, then
`web/index.html` or `mp.py record`): it reports clipped captures per channel.

## 5. Calibrating thresholds

Readings are on a 0..1023 scale, 16 counts per dB; the floor at gain 30 is
about 116. For each pilot:

1. Note the reading with the quad on the ground away from the gate (the
   floor for that pilot) and the peak as it flies through.
2. Set `enter` well above the floor but clearly below the typical peak, and
   `exit` a little below `enter`.
3. Fly a few laps and check every pass counts once.

Each pilot's readings are independent, so pilots can have different
thresholds.

## 6. Frequencies

- Accepted: 5180-5917 MHz in whole MHz. Raceband R1-R8 (5658-5917) is the
  validated set.
- Out of range: Boscam E7 (5925) and E8 (5945).
- Spacing: the receiver's image rejection is about 18 dB at gain 30, so a
  strong transmitter 20 MHz **below** a pilot can show in that pilot's
  reading. Raceband's 37 MHz spacing avoids this; on plans with ~20 MHz
  spacing, check for ghosts. Details in [RF_RESEARCH.md](RF_RESEARCH.md).
- Rate: ~1030 readings per pilot per second with 8 pilots. Fewer pilots
  cycle faster, since each visit takes ~121 us. With very few pilots the
  record rate rises towards what the UART can carry; 8 pilots is the tested
  case.

## 7. Troubleshooting

| Symptom | Check |
|---|---|
| Host never sees a status line | TX/RX crossed over, baud rate, common ground, the C5 powered |
| Status state `ERR_RF` | The radio didn't come up or captures keep failing. Read `status` over USB: is the last boot check `live`? |
| Status `ERR_FREQ` while slots are set | The host's `P` isn't arriving or is rejected (a bad entry rejects the whole list). The `node` console command over USB counts good, bad and over-long lines and rejected commands. |
| All slots read the floor with a VTX on | Antenna path (section 1), VTX actually on that channel (the USB dashboard shows all of Raceband) |
| Readings flat at the top during passes | Gain too high for the distance |
| Steady `seqGaps` on the host | Host-side UART reading: see HOST_IMPLEMENTATION.md section 9 |
