# FPVGate (ESP32-S3) integration

How FPVGate uses the C5. It is a complete, raced host implementation, so
it's a useful reference alongside [HOST_IMPLEMENTATION.md](HOST_IMPLEMENTATION.md).
The code is in the [FPVGate repository](https://github.com/LouisHitchcock/FPVGate);
its user guide is `docs/C5_MULTI_PILOT.md` there. Receiver type 2 in the
config (`receiverRadio`) selects the C5.

## Code map (FPVGate repository)

| File | Role |
|---|---|
| `lib/C5LINK/c5link.{h,cpp}` | UART link: mode detection, `P`/`G`/`Q`, record and line parsing, clock mapping, per-slot sample queues, link counters |
| `lib/C5LINK/c5multipilot.{h,cpp}` | Lap detection for every slot, race laps, race-pilot crossings, peak-hold for the debug popout |
| `src/main.cpp` | `c5Task`; race-pilot crossing to `LapTimer` in `loop()` |
| `lib/LAPTIMER/laptimer.{h,cpp}` | `recordCrossing()`: a crossing from another detector, LapTimer's rules |
| `lib/CONFIG/config.{h,cpp}` | C5 slots and pilot identities (config v24) |
| `lib/WEBSERVER/webserver.{h,cpp}` | `c5Rssi`, `c5Lap`, `c5RssiFast` events; `GET /api/c5/race` |
| `data/c5-multipilot.js` | Calibration tab C5 panel (`C5UI`) |
| `data/script.js` | Race tab: C5 multi-pilot races, history save |
| `data/rssi-debug.{html,js}` | RSSI debug popout, multi-pilot on the C5 |
| `tools/c5_timing_validation.py` | Link and rate validation from the server-sent event feed |

## Servicing the link: `c5Task`

Created at the end of `setup()`: core 1, priority 3 (above `loop()` at 1),
6 KB stack, `vTaskDelay(1)` between passes. Each pass: start the link if
needed, `C5Link::poll()`, start or stop `C5MultiPilot` with the race,
`C5MultiPilot::update()`.

`C5Link::poll()` reads the UART in 256-byte chunks. Reading one byte per call
through Arduino's `HardwareSerial::read()` cost ~20 us a byte, which at
43 KB/s used most of a core and let the 128-deep sample queues overflow. With
bulk reads the task polls every ~1.3 ms (median) and reads ~80 bytes a pass.

Known issue: something stalls `c5Task` for ~60 ms every 5 s (occasionally
~130 ms, which overflows the 4 KB receive buffer and loses one record every
20-40 s). The cause is still being looked for on the FPVGate side.

## Lap detection: `C5MultiPilot`

Per slot, for every sample taken from the link:

1. EMA: `f = (3 f + value) / 4` (on top of the C5's median-of-3 and EMA 0.3).
2. Peak-hold for the debug popout (`takePeakHold`).
3. While a race runs: outside the gate, `f >= enter x 4` enters and starts a
   peak; inside, the highest `f` and its sample time are kept;
   `f <= exit x 4` ends the pass. **The crossing time is the peak's time.**
4. `crossing()` applies LapTimer's rules: the first crossing after the race
   start is Gate 1 (timed from the start); after that each lap runs from the
   previous crossing, and crossings inside the minimum lap time are ignored.
   Crossings before the start are ignored.

Each lap is queued as a `C5LapEvent {pilot, lap (0 = Gate 1), lapTimeMs,
racer}` and stored per slot (up to 64 laps) for `GET /api/c5/race`. All slots
with a frequency produce laps (the Calibration cards show them); `racer`
marks the slots whose laps count in races.

`start(raceStartMs)` takes `LapTimer::getRaceStartMs()` and converts it to
`micros()` by age; it clears the laps.

## Race modes

Racers are the slots with a frequency and the Race switch on (`c5RaceMask`).

| Racers | Mode | How laps reach the race |
|---|---|---|
| 1 | Single-pilot | That racer's raw crossings are queued (`takeRaceCrossing`). `loop()` takes at most one per pass (only when no lap is pending), converts `micros()` to `millis()` by age and calls `LapTimer::recordCrossing()`, which applies Gate 1 and the minimum lap and runs the normal `finishLap`/`startLap`. Everything downstream is unchanged: `lap` event, LCD, RotorHazard, announcer, history. |
| 2-8 | Multi-pilot | `c5Lap` events; the browser builds the race (below). LapTimer still provides the race clock, start, stop and countdown, but records no laps. |

## Config v24 (pilot identity)

New fields at the end of `laptimer_config_t`:

```c
char     c5PilotName[8][21];
char     c5PilotPhonetic[8][21];   // for announcements; empty = use the name
uint32_t c5PilotColor[8];          // 0xRRGGBB
uint8_t  c5RaceMask;               // bit i: slot i races
uint8_t  _reservedC5[3];
```

`EEPROM_RESERVED_SIZE` grew from 832 to 1280 bytes (the struct is ~1072).
Arduino's `EEPROM.begin()` expands the NVS blob in place: it checks NVS has
room, then copies the old bytes into the larger blob and fills the new tail
with 0xFF. Migration from any v10-v23 config calls `initC5PilotIdentity()`:
empty names, the default slot colours, and Race on the slot whose frequency
matches the main pilot frequency (all enabled slots if none matches), which
keeps a single-quad setup working. A sanity pass clears unterminated or
non-printable names and out-of-range colours. A downgrade to v23 firmware
resets the config (its version check rejects v24).

JSON (`/config`, saved by the UI): `c5Pilots` lists all 8 slots
`{id, frequency, enterRssi, exitRssi, name, phonetic, color, race}`; older
pages that send no identity leave it unchanged.

## Web interface

| Event / endpoint | Content |
|---|---|
| `c5Rssi` (10 Hz) | `rssi[8]` (filtered, 0..1023), `freq[8]`, `in[8]`, `on`, `started`, `st`, `mhz`, `gain`, `race`, `pilot`, `ready`, `samples`, `seqGaps`, `queueDrops`, `scan`, `racers` (mask), `multi`, `records`, `badRecords`, `pollGapMaxUs`, `pollBytesMax` |
| `c5Lap` | `{pilot, lap, lapTimeMs, racer}`, sent in batches with `c5Rssi` |
| `c5RssiFast` (25 Hz) | `{v:[8] peak since the last frame, in: mask}`, only between `POST /timer/rssiStart` and `/timer/rssiStop` |
| `GET /api/c5/race` | `{running, racers, multi, pilots:[{slot, frequency, laps:[ms, ...]}]}` |
| `c5Timing` | Slot-mode handshake timings; zero in scan mode |

## Browser

- **Calibration tab (`c5-multipilot.js`, `C5UI`)**: status line, live link
  figures (rates over 2 s, error increases over 10 s), chart, pilot cards
  (channel, name, spoken name, colour, Race, Enter/Exit), auto-calibrate.
  `getRacePilots()` returns the racers for `script.js`.
- **Race tab (`script.js`)**: `c5Race` state. Racers are fixed at the race
  start (`c5RaceReset`). `handleC5RaceLap` stores laps by index and refetches
  `/api/c5/race` if one is missing or the page joined mid-race. The lap table
  reuses `renderMultiPilotRaceView` (one column per pilot); analysis charts
  use `collectAllPilotsData`; announcements are "spoken name, time"; gate
  LEDs use the pilot's colour; max laps per pilot ("finished"), and the race
  stops when all have finished. `saveCurrentRace` writes every racer to
  `pilots[]` and the leader (most laps, then least total time) to the legacy
  single-pilot fields; history renders it as multi-pilot (more than one entry
  in `pilots[]`).
- **RSSI popout (`rssi-debug.js`)**: C5 mode when `receiverRadio` is 2; one
  line per slot from `c5RssiFast` (falls back to `c5Rssi`), chips, the
  selected pilot's thresholds, a racers-only filter. It rereads `/config`
  every 3 s because browser saves don't send `configUpdated` (that event is
  LCD-only).
