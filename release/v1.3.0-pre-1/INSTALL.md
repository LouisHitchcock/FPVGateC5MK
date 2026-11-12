# C5MK 1.3.0-pre-1

**Testing prerelease. The USB soak is NOT PASSED; this is not a production-qualified release.** Stable 1.2.6 remains available.

## What changes

USB to RotorHazard and UART to an FPVGate host now share the same filter: running 95 ms median, smooth 0, merge 300 ms, and jump admission with rise 30 / hold 90 / quiet 100 ms and margin 16 on RotorHazard's 8-bit scale. Protocol 3 wire format is unchanged.

This publishes the previously prepared 1.3.0-rc.1 receiver source with a version-only change to **1.3.0-pre-1**. Receiver/filter implementation files are identical to the archived RC source. The hardware results below were collected on the RC-labelled images; the newly labelled images are build, source and native-regression checked, not newly hardware-qualified.

For matching filtering on the AIO, install [FPVGate 1.9.0-alpha-6](https://github.com/LouisHitchcock/FPVGate/releases/tag/v1.9.0-alpha-6), board **FPVGateC5MKAIO**, on the S3. It includes the shared median and the clock-age fix that keeps all eight slots live, plus the Wi-Fi loading fix. Update both S3 app and filesystem when coming from older firmware. This C5 package does not flash the S3 and includes no companion S3 images.

## Images

| Directory | Board | C5 UART TX / RX | Hardware evidence |
| --- | --- | --- | --- |
| `xiaoc5/` | Seeed XIAO C5, including standard C5MK AIO | GPIO24 / GPIO23 | RC source tested over USB and factory-wired AIO UART; limitations below |
| `c5zero/` | Waveshare C5-Zero receiver | GPIO4 / GPIO5 | Build only; no receiver hardware qualification |
| `xiaoc5_uart1_d6d7/` | One modified AIO with D6/D7 bodge wires only | GPIO11 / GPIO12 | Build only; **never use on a standard AIO** |

The online C5 flasher offers the normal XIAO/C5-Zero images. The D6/D7 image is a separate manual download for that modified board only.

## Installing the C5

Use the **C5's own USB port** with esptool 5.1 or newer (ESP32-C5 support). Check the detected capacity with `flash-id`, then back up the full flash. Replace `SIZE` with that capacity in bytes (for example, `0x800000` for 8 MB):

```text
python -m esptool --chip esp32c5 --port PORT flash-id
python -m esptool --chip esp32c5 --port PORT read-flash 0 SIZE backup.bin
```

Run from the chosen board directory:

```text
python -m esptool --chip esp32c5 --port PORT --baud 921600 write-flash 0x2000 bootloader.bin 0x8000 partitions.bin 0xf000 ota_data_initial.bin 0x20000 firmware.bin
```

These writes leave NVS at 0x9000 intact. Avoid a full erase if retaining settings. Power cycle after flashing. Existing protocol-3 receivers may alternatively receive the board-matched application through the S3's C5 updater. Recalibrate and check deliberate passes before racing.

Never use these C5 offsets or images on the S3. The standard AIO uses `xiaoc5`, not D6/D7. Verify the C5 is online with `1.3.0-pre-1` and board `xiaoc5`; on the S3, `/api/rh/info` should show `filter.kind = shared-median`, `medianMs = 95`, `extraHostEma = false`. Standard AIO S3 link pins are TX4/RX5.

## Validation and limits

- Offline RC filter parity: **11,798,808 checks in 28 cases**, four retune-window regressions, unchanged legacy-mode output. The paired S3 clock-mapped regression keeps all eight slots live.
- Exact RC USB soak: first three heats **192/192** matched; heats 4-5 **52/64 and 51/64** after an abrupt signal change on both receivers. Every miss had a peak below Enter; fixture versus receiver cause remains unresolved. **Soak NOT PASSED.**
- Completed clean_a, clean_b, normal_a and normal_b USB heats each detected **6/6 with zero extras**. Clean interval MAE improved, but **moderate-noise timing was worse than stock**: 88.8 / 29.0 ms versus 51.4 / 14.8 ms. Scored noisy stress heats were not run after a rig serial failure.
- Factory-wired AIO UART: eight samples per record, about 8.2k samples/s; short capture windows and a two-minute idle capture had all link/UART error counters at zero. Rig heats had zero extras at normal thresholds, but rig output limited higher channels. Temporary per-slot thresholds produced 39/48 detections and two extras; settings were restored. One scripted source sharing a transmitter is not eight independent VTXs.
- Air65 R1 / 25 mW: a 600-second ON/OFF signal capture kept the link online with zero error counters. It did **not** score walking laps or independently confirm active camera video. It is not pass-accuracy validation.
- **Real analog-video walking/flying passes, long-duration qualification, C5-Zero receiver hardware and the D6/D7 variant remain unqualified.** A prior C5-offline startup recurrence recovered after a full power cycle and remains unresolved.
- Comparisons used one candidate board against a simultaneous stock board without a board crossover. USB receipt alignment supports interval comparisons, not absolute-latency claims. No general timing-accuracy or RX5808-equivalence improvement is claimed.

The provisional 10 November S3 companion beginning `09d9ef2c` has the slot-age/RSSI bug and must not be used. Use the separately published Alpha-6 S3 images instead.

Checksums, exact source/build provenance and board-specific flash instructions accompany these images. Do not infer that a successful build or a prerelease label closes the remaining qualification gaps.
