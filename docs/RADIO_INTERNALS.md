# Radio internals

How the firmware drives the ESP32-C5's radio to take raw I/Q captures and
measure each pilot. Code: `multipilot/src/radio.cpp`, `dump.cpp`,
`meter.cpp`. Where each fact came from is in [PROVENANCE.md](PROVENANCE.md).

None of this is documented by Espressif. It was worked out from Espressif's
Apache-2.0 libraries and checked on chip v1.0. Other chip revisions or
ESP-IDF versions may differ: re-run the checks in the last section after any
change of chip, ESP-IDF or PHY library.

## 1. Overview

The C5's PHY has an **ADC dump engine**. On a trigger it writes N
consecutive baseband samples (10-bit I, 10-bit Q and the live gain index,
packed into one 32-bit word per sample) at **80 MS/s** into a fixed SRAM
area. For each pilot the firmware:

1. retunes the LO to 10 MHz below the pilot, PLL only;
2. triggers one capture (256 samples, 3.2 us of RF, in scan mode);
3. removes DC, mixes the pilot down to 0 Hz and sums Hann-windowed blocks of
   8 samples, giving the power in a ~10 MHz band around the pilot;
4. filters that dB value (median of 3, then EMA).

The receiver gain is forced to a fixed index; the native AGC is unused.
Nothing transmits.

## 2. Build environment

From `sdkconfig.defaults`:

| Setting | Why |
|---|---|
| `CONFIG_ESP_PHY_ENABLE_CERT_TEST=y` | The dump trigger `adctrig` and `force_rx_gain` live in `librftest.a` (object `mac_common.o`), which ESP-IDF only links with this option. The Arduino-ESP32 prebuilt libraries don't include it, which is why this is an ESP-IDF project. |
| `CONFIG_ESP_PHY_DISABLE_PLL_TRACK=y` | The periodic PLL-tracking timer would otherwise retune the radio under the scanner. |
| Wi-Fi/PHY IRAM optimisations, AMPDU and power management off | Keeps static data small (below). |
| `CONFIG_LOG_DEFAULT_LEVEL_WARN=y` | `adctrig` prints four hex values through `phy_printf` at INFO; WARN silences it. |
| `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n` | The scan task yields only every 50 cycles. |
| 240 MHz, FreeRTOS 1000 Hz, `-O2` | Speed |

`librftest.a`'s `wifi.o` refers to `cmd_parse`, which only Espressif's RF
test console defines. `radio.cpp` provides a stub returning -1 in case the
linker pulls that object in; with ESP-IDF 5.5 it doesn't.

**The SRAM bank.** The engine writes from `0x40830000` upward, and while a
capture runs the modem, not the CPU, owns the SRAM. `dump.cpp` reserves the
whole 128 KiB bank `0x40820000`-`0x4083FFFF` from the heap with
`SOC_RESERVE_MEMORY_REGION`. `check_sram.py` runs after every build and fails
it if `_iram_end`, `_data_end` or `_bss_end` passes `0x40820000`, or if
`adctrig` or `force_rx_gain` is missing. Keep large buffers on the heap; the
current build leaves ~9 KB of headroom.

## 3. The capture trigger and why it is receive-only

`adctrig` takes nine 32-bit arguments:

| Arg | Meaning | Value used |
|---|---|---|
| 1 | sample count minus 1 | N - 1 |
| 2 | trigger/source selector | **0** (software trigger) |
| 3 | mode | **0** |
| 4 | rate code (rate field = code/2 when code > 1) | **0** = 80 MS/s |
| 5 | unused here | 0 |
| 6 | wait style | **0** |
| 7-9 | inputs to the wait-style paths | 0 |

Paths that must never run, and why the values above avoid them:

- `mode` 1 powers the DPD path and calls `phy_start_tx_tone_step`: a **tone
  transmit**.
- `selector` 5 or 6, or `mode` 2, calls `trig_tx_frame`: a **frame
  transmit**.
- `selector` 12 starts the BLE receiver.
- Wait styles 1-3 write a gain field or add a timed delay.

`dump.cpp` calls `adctrig` in exactly one place, `rxOnlyStockDump()`, with
these values fixed. Nothing else, in particular not the console, can change
them.

## 4. The direct trigger

The stock `adctrig` call costs ~1.65 ms of fixed overhead per capture. After
one stock call has programmed the engine's setup registers, the firmware
drives the same register sequence itself, with interrupts masked:

| Register | Fields |
|---|---|
| `0x600A9004` control | count bits 0-16, done bit 18, trigger bit 19, enable bit 31; bits 17, 20, 21 cleared |
| `0x600A9008` setup | rate bits 21-23, source bits 17-20 (left as the stock call set them) |
| `0x60095004` SRAM ownership | bits 8-11 = 2 and bit 16 set: the modem owns the bank |

1. Save `0x60095004`; set bits 8-11 to 2 and bit 16.
2. Write the control register: count = N, enable set.
3. Pulse the trigger bit (set, then clear).
4. Poll the done bit, timing out after 1 ms.
5. Clear enable; clear the ownership bits, then restore the saved value.

On a timeout the firmware falls back to the stock call (in practice: never).
Before each capture it writes a sentinel to words 0..N-1 and guard values to
N..N+3; afterwards a capture is rejected if the last word still holds the
sentinel or a guard changed. A 256-sample direct capture takes ~10 us, and
direct and stock captures give the same readings.

## 5. The sample word

| Bits | Content |
|---|---|
| 0-9 | I, signed 10-bit |
| 10-19 | Q, signed 10-bit |
| 20-26 | the gain index in force for this sample |

- **The spectrum is inverted:** RF above the LO appears at negative
  frequency in an FFT of I + jQ. One named constant, `kSpectrumSign` in
  `meter.h`, handles it, and the meter, `mp.py` and the dashboard all use it.
  Get this wrong and the meter reads the empty mirror instead of the pilot.
- DC offset with nothing on air: about -1.5 to -2.5 LSB on I and Q at gain
  30-36. Removed per capture.
- Values at the rails (>= 511 or <= -512) mean the ADC clipped.

## 6. Tuning

1. **Band setup**, once at boot and whenever the bandwidth mode changes:
   `phy_set_chanfreq(5180, 1)`. It calls Espressif's `phy_chip_set_chan`,
   which calibrates the 5 GHz band. The second argument selects the analog
   bandwidth; 1 (wide) is used.
2. **Each hop: the PLL only.** `phy_set_rf_freq_offset(xtal, MHz, trim)`,
   with `xtal` = byte 49 of `phy_param` and `trim` = the int16 at byte 30,
   the values Espressif's own channel code passes. Integer MHz only.
3. **Without the fixed wait.** `phy_set_rf_freq_offset` ends with a fixed
   300 us delay. `radio::hopNoWait` makes the same exported calls in the
   same order without it (and stores the MHz in `phy_param` as the original
   does): 68 us instead of 398, and readings match the steady state straight
   away. Skip mask 3 also leaves out the 5G clock and frequency-memory
   updates, which made no measurable difference on Raceband. Details and
   caveats in [RF_RESEARCH.md](RF_RESEARCH.md).

A full retune through `phy_set_chanfreq` takes ~1.2 ms; it isn't needed per
hop.

## 7. Gain

- `force_rx_gain(1, index, 0)` writes the index to bits 24-31 of
  `0x600A702C` and the force-enable flag to bit 23, with no delay. Don't use a
  nonzero third argument: it takes a Bluetooth path with other registers and
  a 100 us delay.
- The highest calibrated index is in bits 8-14 of the same register: 89 on
  chip v1.0. The firmware reads it and never forces higher.
- The gain is re-forced after every hop (one register write). The gain field
  in every sample word confirms it held.

## 8. Bring-up order

1. NVS, the default event loop, then Wi-Fi: mode NULL, start, power save
   off, promiscuous on, channel 1. This keeps the PHY powered for captures.
2. Band setup (section 6).
3. A boot check: one stock capture, which must come back complete with a
   plausible level and the right gain field. The firmware then turns the
   transmit path off (`phy_pbus_workmode`, `phy_pbus_xpd_tx_off`) as a safety
   measure and checks again. Only if captures are still flat does it call
   `phy_pbus_xpd_rx_on(1)` and `phy_set_rxclk_en(1)`. `status` on the
   console shows the three checks.
4. Force the gain, tune, and seed the engine with one stock capture; direct
   captures follow.

## 9. The band-power meter

- **LO plan:** LO = pilot - 10 MHz. With the inversion the pilot appears at
  -10 MHz. That keeps it well off DC, leaves its own mirror (+10 MHz) empty,
  and puts Raceband neighbours (37 MHz away) at -47 and +27 MHz, outside the
  meter's band. Never put the LO midway between two pilots: each lands on
  the other's mirror.
- **Per capture:** decode I and Q; multiply by a precomputed table that
  shifts -10 MHz to 0 Hz with a Hann window of length 8 folded in (Q14);
  sum each block of 8 products (each block is one windowed DFT bin, about
  10 MHz wide, centred on the pilot); subtract each block's response to the
  capture's mean (DC); average |block|^2 and convert to dB. Normalised so an
  in-band tone of amplitude A reads A^2 (LSB^2).
- **Speed:** at a 10 MHz offset and 80 MS/s the mixer repeats every 8
  samples, so every block uses the same 16 coefficients. `powerPeriodic()`
  keeps them in registers and the DC correction becomes one value: 33 us for
  256 samples. `measure()`, used by the USB scanner, does the same sum in two
  passes and also reports DC, clipping and the gain field; the PC tests check
  both give identical results.
- **Filter:** median of the last 3 dB values (removes single-capture spikes,
  such as 5 GHz Wi-Fi bursts), then an EMA with alpha 0.3.
- **Why band power:** a single-frequency measurement catches only a slice of
  an FM video signal, and swings with picture content. A ~10 MHz window holds
  most of the energy: filtered readings on live quads vary by 0.26-0.47 dB
  standard deviation.

## 10. Checks after porting or upgrading

Run these on the USB console (`tools/mp.py --port <port> cmd "..."`) after
changing the chip, board, ESP-IDF or PHY library:

| Check | Command | Expect (chip v1.0, gain 30) |
|---|---|---|
| Build | `pio run` | `check_sram` passes; `adctrig` and `force_rx_gain` present |
| Bring-up | `status` | `wifi=1`, `gainmax` ~89, last boot check `live` |
| Timing | `timer` | PLL hop ~400 us, full setup ~1.2 ms, stock capture ~1.7 ms, direct capture well under 0.1 ms for 2048 samples, 0 timeouts |
| Stock against direct | `cmp` | Means within ~0.2 dB, same spread |
| SRAM ownership | `sramtest` | 0 failed captures. It also reports whether the lower half of the bank changed; the firmware reserves the whole bank either way. |
| Noise floor, no VTX | dashboard or `mp.py record` | about -3 dB on all channels, steady |
| Spectrum orientation | `capture <LO>` with a VTX 10 MHz above the LO, or the dashboard's spectrum view | One peak, shown at +10 MHz from the LO (the view corrects the inversion); mirror 15-35 dB lower; gain field constant |
| Hop settling | `hopprof <MHz>` with a VTX on | Readings at 0 us delay match steady state |
| Scan rate | `nodeprof` while the host scans 8 pilots | ~121 us per visit |
