# C5 scan engine

How the C5 measures up to eight pilots ~1000 times a second each. Code in
`multipilot/src`. The radio mechanism (capture engine, tuning, gain, meter)
is in [RADIO_INTERNALS.md](RADIO_INTERNALS.md); this page covers how the
firmware schedules it and why it is fast.

## Modes

`main.cpp` runs one task (`scan`, priority 10; the C5 is single-core):

| Mode | Entered by | What it does |
|---|---|---|
| USB scanner | Boot default | Cycles `cfg.pilots` with `visit()` (stock hop, n=2048, settle) and sends `R` frames to the USB dashboard (`web/index.html`, `tools/mp.py`) |
| Node, single frequency | `F` from the host | `node::Core` state machine: retune, `OK`, `R` lines at 1 kHz |
| **Node, scan** | `P` from the host | `Core::scanCycle()` every pass: all slots, one record per cycle |

The first valid line from the host makes the node active and pauses the USB
scanner (`scan on` on the console takes the radio back). `node off` ignores
the host.

## One pilot visit (scan mode)

`Core::scanCycle()` calls `NodeRadio::hop()` and `NodeRadio::measure()` in
`main.cpp`:

1. `radio::hopNoWait(pilot - 10 MHz, skip = cfg.nodeHopSkip)`: the PLL
   steps without the fixed 300 us wait (see RF_RESEARCH.md). Skip mask 3 also
   leaves out the 5G clock and frequency-memory updates. **68 us.**
2. `radio::forceGain(gain)`: one register write.
3. `dump::capture(256, Direct)`: direct-triggered I/Q capture, 256 samples =
   3.2 us of RF at 80 MS/s, with sentinel and guard checks. **10 us.** The C5
   timestamp is the middle of the capture.
4. `BandMeter::power()`: band power around the pilot. **33 us.**
5. `PilotFilter::push()`: median of 3 (removes single spikes), then EMA,
   alpha 0.3; `rssiFromDb()` maps to 0..1023.

Measured with `nodeprof` (8 Raceband pilots, n=256, skip 3):

| | us per visit |
|---|---|
| hop | 68.1 |
| capture | 10.1 |
| power | 33.0 |
| between visits (filter, record field) | 4.8 |
| cycle boundary, amortised (record write, main loop) | ~6 (47.6 per cycle) |
| **total** | **~121**, so ~1030 Hz per pilot for 8 pilots |

The budget for 1 kHz x 8 pilots is 125 us per visit, so there is little
headroom. The cheapest further gains: `noden 192` or `noden 128` (shorter
captures, more noise), or trimming the hop's I2C writes.

## The band-power meter (`meter.cpp`)

The pilot sits 10 MHz above the LO and, with this I/Q layout, appears at
-10 MHz in the capture (`kSpectrumSign`). For each block of 8 samples the
meter multiplies by a Hann window and a mixer that brings the pilot to 0 Hz,
and sums: each block is one windowed DFT bin about 10 MHz wide centred on the
pilot. DC is removed using the capture mean (the block's response to DC is
precomputed). Power is the mean |block|^2, in dB.

`measure()` (the USB scanner) does this in two passes and also reports DC,
clipping and gain-field checks. `power()` (the node) gives **exactly the same
result** in one pass: block sums are kept and the DC correction applied
afterwards. When every block uses the same coefficients (true for a 10 MHz
offset at 80 MS/s, where the mixer repeats every 8 samples),
`powerPeriodic()` keeps the 16 coefficients in registers and the DC
correction is a single value. Both run from IRAM at `-O3`. Progress: 66 us
(two passes), 41 (one pass), 38 (IRAM, O3), 33 us (periodic). The C5 has no
FPU, so `log10f`, the filter and the dB mapping are soft-float; together they
cost a few us.

## Things that mattered for speed

| Change | Effect |
|---|---|
| `hopNoWait` instead of `phy_set_rf_freq_offset` | 398 to 68 us per hop |
| n=256 instead of 2048 | capture 55 to 10 us, meter 422 to 66 us |
| Single-pass, then periodic meter | 66 to 33 us |
| The node's USB view sends without waiting (`hostlink::send(..., 0)`) | A 2 ms wait every 20 ms on a full USB buffer took **10%** of the scan when no dashboard was reading |
| USB console polled every 16th pass in node mode | Fewer driver calls per cycle |
| `nodelink::poll()` only reads the UART when bytes are waiting | `uart_get_buffered_data_len` is much cheaper than an empty `uart_read_bytes` |
| UART check once per cycle, not per visit | |
| USB view `onSample` every 16th cycle | It only shows the latest value at 50 Hz |

## USB console

Over the C5's native USB: `tools/mp.py --port <port> cmd "..."`, or
`mp.py console` for an interactive session. `help` lists everything.

| Group | Commands |
|---|---|
| General | `status`, `regs`, `scan on/off`, `pilots rb` or `pilots <MHz,...>`, `gain N`, `offset MHz`, `n N`, `settle us`, `alpha A`, `tune pll/full`, `dump direct/stock`, `bw 0/1` |
| Checks | `timer` (hop, capture and meter timings), `cmp [count]` (stock against direct captures), `capture MHz [n]` (raw capture to the host), `sramtest` |
| Node | `node` (state and counters, including `scan mask`, `cycles`, `aborted`), `node on/off`, `node echo on/off` (copy the node's UART output to USB), `node map lo hi`, `node alpha A`, `node cmd <F,...;G,...;P,...;Q>` (inject commands as if from the host) |
| Scan mode | `noden N` (samples per capture, default 256), `nodeskip 0-3` (hop skip mask, default 3), `nodeprof` (per-visit timing since the last call) |
| RF research | `hopprof MHz [jump] [skip]`, `hopspikes MHz [count] [how]`, `bandshape MHz` (see RF_RESEARCH.md) |

Most commands use the radio and pause scanning while they run. With a host
attached, `node off` first and `node on` afterwards.

## Build notes

- ESP-IDF via the pioarduino PlatformIO platform (`framework = espidf`).
  Settings and why: RADIO_INTERNALS.md section 2.
- `check_sram.py` fails the build if static data reaches the capture bank;
  the current build has ~9 KB of BSS headroom.
- `platformio_local*.ini` files (gitignored) are picked up if present, for
  machine-specific settings such as a toolchain override.
- Per-board link pins go through `board_build.cmake_extra_args` and
  `src/CMakeLists.txt`: PlatformIO's `build_flags` don't reach the project
  sources in this ESP-IDF build.
