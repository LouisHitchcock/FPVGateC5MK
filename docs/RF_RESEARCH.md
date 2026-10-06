# RF research findings

What was measured on the bench in October 2026, and what it means. Board:
Waveshare ESP32-C5-Zero, chip v1.0, gain index 30, bandwidth mode 1,
Raceband pilots, unless stated otherwise (section 8 compares the Seeed
Studio XIAO ESP32-C5). Transmitter tests used a bench VTX in short bursts.
Function names and steps come from our own disassembly of Espressif's
Apache-2.0 `libphy.a` for the ESP32-C5 (`riscv32-esp-elf-objdump -d -r`).

## 1. Where the time went (baseline)

`timer` console command, VTX off:

| | n=2048 | n=1024 | n=512 | n=256 |
|---|---|---|---|---|
| PLL hop (`phy_set_rf_freq_offset`) | 398 us | 398 | 398 | 398 |
| Direct capture | 55 | 29 | 16 | 10 |
| Meter (`measure`, two passes) | 422 | 219 | 117 | 66 |
| Stock capture (`adctrig`) | ~1720 | ~1690 | ~1680 | ~1675 |
| Full retune (`phy_set_chanfreq`) | ~780 (1.2 ms max) | | | |
| **Per pilot, 8 pilots** | 139 Hz | 186 Hz | 225 Hz | 251 Hz |

The hop was a flat 398 us with almost no spread, and 80% of every visit even
at n=256. A fixed figure like that pointed at a delay, not a lock time.

## 2. The PLL hop: a fixed 300 us wait

`phy_set_rf_freq_offset(xtal, MHz, trim)` is:

```text
phy_set_rfpll_freq(xtal, MHz, trim, &sdm)
    phy_rfpll_set_freq(MHz, xtal, trim, &sdm)   compute the PLL settings
    phy_write_rfpll_sdm(&sdm)                    I2C writes, block 0x63
    phy_set_freq_i2c_new(MHz)                    dcap, xtal code, VCO init
    phy_restart_cal()                            toggles block 0x63 reg 0 bit 0
    phy_i2c_sdm_en(5G flag)                      SDM reset, two 5 us waits
    ets_delay_us(300)                            <-- tail call: a fixed wait
if 5G: phy_ckgen_5g_cal(MHz)                    3 I2C writes, block 0x65, + ets_delay_us(20)
if 5G or MHz > 2482: phy_freq_mem_change_5g_(&sdm)
```

All of these are exported from libphy. `radio::hopNoWait(mhz, stageUs, skip)`
makes the same calls in the same order without the 300 us tail. It stores
the MHz in `phy_param` (uint16 at byte 714) as `phy_set_rfpll_freq` does.
Skip bit 0 leaves out `phy_ckgen_5g_cal`, bit 1 `phy_freq_mem_change_5g_`.

`hopprof` stage timings (average):

| Stage | us |
|---|---|
| rfpll_set_freq + write_rfpll_sdm + set_freq_i2c_new | 33.8 |
| restart_cal | 8.3 |
| i2c_sdm_en (includes 10 us of waits) | 22.1 |
| ckgen_5g_cal (includes 20 us wait) | 26.4 |
| freq_mem_change_5g_ | 10.7 |
| **total, skip 0** | **103** |
| **total, skip 3** | **68** |

Espressif's own channel set (`phy_set_channel_rfpll_freq_new`) follows
`phy_set_rf_freq_offset` with `phy_wait_rfpll_cal_end_new`. That routine
writes block 0x63 reg 15, restarts calibration, waits 20 us, reads reg 11 and
retries up to 10 times until the value is in a window (125-130 in its mode).
Read straight after `hopNoWait`, reg 11 held a constant per-frequency value
(78 at LO 5722, 54 at 5907, 87 at 5648) from the first read. It is not a lock
indicator in this use; the VCO calibration had already finished.

### Settling, with a carrier (R1, 5658)

`hopprof 5658 37 <skip>`: hop in from 37 MHz below with `hopNoWait`, wait
0-300 us, take a 256-sample capture, 12 repeats per delay, compared with the
steady state after a full hop plus 2 ms.

- Both skip 0 and skip 3 read within ~0.2 dB of steady state **from 0 us**,
  with the same 0.2 dB spread. The PLL is settled when `hopNoWait` returns.
- Skipping `ckgen_5g` and `freq_mem` made no visible difference on R1.

So a hop costs 68 us, not 398. Caveat: settling was checked only on R1,
coming from 37 MHz below. The scan also jumps R8 to R1 (259 MHz) every cycle,
and skip mask 3 relies on whatever `ckgen`/`freq_mem` state the last full
tune left. Both look fine in use but haven't had a dedicated carrier test
(section 9).

## 3. Post-hop spikes

About 1 in 25 captures in the settling runs read ~30 dB high. `hopspikes
5658 500 <how>` counted them (reading > median + 6 dB):

| Hop method | Carrier on (R1) | Carrier off |
|---|---|---|
| No hop | 0 / 500 | 0 / 500 |
| Stock hop | 4 / 500 | 0 / 500 |
| hopNoWait, skip 0 | 10 / 500 | 0 / 500 |
| hopNoWait, skip 3 | 4 / 500 | 0 / 500 |

The spikes read 12-25 dB above normal, with no clipping and the gain field
unchanged in every sample. They come from hopping in general, not from
removing the wait (10 against 4 is within chance), and only appear with a
signal present. The cause is unknown; a raw capture of a spike would show
it. The median-of-3 filter removes isolated spikes, and at ~1% they don't
affect laps.

## 4. Passband and image rejection

`bandshape 5658` with the VTX on R1: LO stepped from 38 MHz below to 38 MHz
above the carrier, band power at the carrier's offset and at its mirror,
n=2048, gain 30, bw 1. Noise floor about -3 dB.

| Carrier offset from LO | Signal | Against centre | Mirror | Rejection |
|---|---|---|---|---|
| +-2 to +-15 MHz | 15.4-15.9 dB | flat | (overlaps within +-8) | ~18 dB beyond +-9 |
| -18.5 | 14.2 | -1.3 dB | -2.9 | 17.1 dB |
| +19 | 13.1 | -2.6 dB | -2.9 | 15.9 dB |
| -21.5 / +20.5 | 11.8 / 12.5 | ~-3.5 dB | | ~15 dB |
| -24.5 / +25 | 8.4 / 6.4 | ~-8 dB | | 9-11 dB |
| +-30 | ~1 | ~-15 dB | | ~4 dB |
| +-36 | ~-2 | at the floor | | |

With the carrier off the floor is flat across +-38 MHz: it is ADC and
baseband noise, not front-end noise shaped by the analog filter.

What it means:

- **Usable instantaneous bandwidth about +-20 MHz** (-3 dB near +-21 MHz).
- **Image rejection about 18 dB at gain 30**, lower than the 25-36 dB seen in
  raw captures at gain 36. A strong signal 20 MHz below a pilot (the pilot's
  mirror with a 10 MHz LO offset) can show in that pilot's reading.
  Raceband's 37 MHz spacing keeps neighbours clear; ~20 MHz-spaced plans may
  see ghosts.
- **Two pilots per capture was considered and rejected for now.** With the
  LO midway between a Raceband pair (+-18.5 MHz), each pilot's image lands on
  the other at only ~17 dB down. It would halve the hops, but needs digital
  IQ-imbalance correction (typically 35-45 dB) first.

## 5. Frequency range

- The node accepts **5180-5917 MHz** for a pilot (LO 5170-5907). The USB
  scanner accepts 5100-5950 for experiments.
- Band setup (calibration) runs at 5180 MHz.
- Checked with a carrier: R1, R7, R8. 5917 reads a normal floor. Nothing
  above 5917 tested. Lowband (5362-5621) is accepted but untested with a
  carrier.
- Not reachable: Boscam E7 (5925) and E8 (5945).

## 6. Getting to 1 kHz per pilot

60 s runs, 8 Raceband pilots, measured on the host (FPVGate's
`tools/c5_timing_validation.py`):

| Step | Per pilot | Notes |
|---|---|---|
| Host drives 16 ms slots with F/OK | 116 Hz | Bursts of ~14 readings, then ~112 ms blind |
| C5 scan mode + binary records | 761 Hz | hopNoWait skip 3, n=256, two-pass meter |
| Single-pass meter | 904 Hz | |
| USB view no longer waits on a full buffer | 987 Hz | That wait took 10% of the scan |
| Lighter C5 main loop | 990 Hz | |
| Periodic meter (coefficients in registers) | 1032 Hz | The host then dropped samples |
| Host link task with bulk UART reads | **1028-1032 Hz, clean** | 0 gaps, 0 drops |

### The earlier slot design

Before scan mode, the host retuned the C5 per 16 ms slot. Handshake phases:
F sent to TUNING received 1.2 ms (median), TUNING to OK 0.46 ms, OK to the
first R 0.32 ms; F to the first R 2.1 ms. Settle times of 100, 50 and 25 us
made no difference to the total. 12 ms slots were slower than 16 ms (906
against 930 total readings/s) because the fixed handshake cost was spread
over fewer samples. That ceiling is why the scan moved onto the C5.

## 7. Ideas not pursued yet

- Trim the hop further: direct register I2C instead of the
  `phy_i2c_writeReg_Mask` wrappers (~3-4 us each), or skip
  `phy_set_freq_i2c_new` parts that don't change across Raceband.
- Digital IQ-imbalance correction, for better image rejection and two pilots
  per capture.
- A second C5 (four pilots each) if ever more rate is needed.
- Fixed-point log and filter on the C5 (no FPU).
- Lap accuracy depends on more than sample rate: a gate pass is hundreds of
  ms wide, and peak timing on a 1 kHz stream is already well inside 1 ms of
  quantisation.

## 8. Second board: Seeed Studio XIAO ESP32-C5

The `xiaoc5` build was tested on a XIAO ESP32-C5 (chip v1.0) with an
external antenna, linked to an FPVGate XIAO ESP32-S3 on D4/D5. Same firmware
as the C5-Zero apart from the link pins.

| | XIAO ESP32-C5 | C5-Zero |
|---|---|---|
| Bring-up | `wifi=1`, `gainmax=89`, captures live without the RX-on calls | `wifi=1`, `gainmax=89` |
| `timer`, n=2048: hop / direct capture / meter | 398 / 55 / 422 us | 398 / 55 / 422 us |
| Floor, 8 Raceband channels, gain 30 (USB scanner) | -1.7 to -1.9 dB, filtered sd 0.11-0.14 dB | about -2.3 dB, sd 0.14-0.23 dB |
| DC offset with nothing on air (I / Q) | about -5 / +10 LSB | about -2 LSB |
| Through FPVGate, 8 pilots, gain 31 | 1031 Hz per pilot, 0 bad records, 0 gaps, 0 drops | 1028-1032 Hz, same |
| Idle level through FPVGate, gain 31 | 129-133 counts on every slot | |
| VTX on R8 (bench distance) | R8 rose to 660 counts (+33 dB), steady within 4 counts; R1-R7 unchanged | |
| VTX off | R8 back at the floor from the first reading | |
| C5 power-cycled with FPVGate running | FPVGate resumed scanning on its own, no errors | |

The larger DC offset makes no difference to the readings: the meter removes
DC from every capture. On this evidence the two boards are interchangeable.

## 9. Known limitations and open questions

1. **Post-hop spikes** (section 3): ~1% of captures, carrier only, cause
   unknown. Hidden by the median-of-3.
2. **Skip mask 3 and settle-free captures** were carrier-checked only on R1
   from 37 MHz below; not yet across the band or on the R8 to R1 jump.
3. **Image rejection ~18 dB** at gain 30: ghosts are possible on ~20 MHz
   spaced plans.
4. **Above 5917 MHz** untested (E7 and E8 unreachable); Lowband untested
   with a carrier.
5. **Lap accuracy** against video or a stopwatch not measured yet.
6. **Multi-pilot races** with two or more real pilots flying at once not run
   yet; the near/far case (one quad at the gate, another far away, does the
   near one's clipping disturb the far one?) not tested.
7. **Gain policy** for real race distances (fixed per event, per pilot, or
   stepped) not settled.
