# System architecture

How a timer with an ESP32-C5 receiver turns RF into laps. The host described
here is FPVGate on an ESP32-S3; any other host follows the same split (see
[INTEGRATION_GUIDE.md](INTEGRATION_GUIDE.md)).

## The two chips

| | ESP32-C5 (this repo) | Host: ESP32-S3 running FPVGate |
|---|---|---|
| Role | RF front end: measures every pilot's signal | Controller: lap timing, races, web app, storage |
| Work | Retunes its own 5 GHz radio across up to 8 pilots, takes an I/Q capture per pilot, turns it into a band-power reading | Parses readings, detects gate passes, runs races, serves the UI |
| Rate | ~1030 readings per pilot per second with 8 pilots | Processes ~8.2 k readings/s |
| Link | UART1, 921600 baud 8N1 (pins per board, see the README) | Serial1, GPIO5 RX / GPIO4 TX (XIAO D4/D3) |
| Dev link | Native USB: flashing, `mp.py` console, dashboard | Wi-Fi web app, OTA |

## Data flow

```text
 C5 (scan task, single core, priority 10)
 +--------------------------------------------------------------+
 | for each slot with a frequency:                              |
 |   hopNoWait(LO = pilot - 10 MHz)   68 us   (radio.cpp)       |
 |   forceGain(gain)                                            |
 |   direct I/Q dump, 256 samples     10 us   (dump.cpp)        |
 |   BandMeter::power()               33 us   (meter.cpp)       |
 |   median-of-3 + EMA (alpha 0.3) -> 0..1023                   |
 | one binary record per cycle (~1 ms)        (node_core.cpp)   |
 +------------------------------+-------------------------------+
                                | UART 921600, ~43 KB/s
 Host (S3)                      v
 +--------------------------------------------------------------+
 | c5Task (core 1, priority 3, every 1 ms)                      |
 |   C5Link::poll: bulk UART read, record parse, CRC,           |
 |     C5 clock -> micros(), per-slot sample queues (128)       |
 |   C5MultiPilot::update: EMA 3/4, Enter/Exit hysteresis,      |
 |     peak timing, per-slot laps, race-pilot crossings         |
 +--------------------------------------------------------------+
 | loop() (core 1, priority 1)                                  |
 |   race-pilot crossing -> LapTimer::recordCrossing (1 racer)  |
 |   LapTimer laps -> "lap" event, LCD, RotorHazard, announcer  |
 +--------------------------------------------------------------+
 | web (async_tcp)                                              |
 |   c5Rssi (10 Hz), c5Lap, c5RssiFast (25 Hz while open)       |
 |   GET /api/c5/race, /config (c5Pilots)                       |
 +------------------------------+-------------------------------+
                                | server-sent events
 Browser                        v
   c5-multipilot.js (Calibration tab), script.js (Race tab, history),
   rssi-debug.js (RSSI popout)
```

## Why it is built this way

- **The C5 schedules the scan itself.** An earlier design had the host retune
  the C5 with an `F`/`OK` handshake per 16 ms slot: each pilot got ~14
  readings and was then blind for ~112 ms (116 readings/s per pilot). Moving
  the loop onto the C5 removed the handshake and the blind spots.
- **Binary records.** ASCII `R,seq,value*HH` lines for 8 k readings/s need
  ~112 KB/s, more than 921600 baud carries. One 42-byte record per cycle is
  ~43 KB/s.
- **C5 timestamps.** Each reading carries the C5's time at the middle of its
  capture; the host maps that onto its own clock. Lap times don't depend on
  UART or host loop latency.
- **A dedicated host task with bulk reads.** At 43 KB/s, reading the UART a
  byte at a time through Arduino's driver used most of a core.

## Clocks and time bases

| Clock | Where | Used for |
|---|---|---|
| C5 `esp_timer` (us) | Record `t0` and per-slot `dt` | Sample times |
| S3 `micros()` | Sample queues, peaks, crossings | Lap detection |
| S3 `millis()` | `LapTimer`, race start | Race clock, single-pilot laps |

The host maps C5 time to `micros()` (see [LINK_PROTOCOL.md](LINK_PROTOCOL.md)).
FPVGate's `C5MultiPilot::start()` converts the race start from `millis()` to
`micros()` by age, and `loop()` converts a crossing back to `millis()` by age,
because the two clocks wrap at different points.

## Where things live

| Topic | Document |
|---|---|
| UART protocol, records, clock mapping | [LINK_PROTOCOL.md](LINK_PROTOCOL.md) |
| Building the C5 into another system | [INTEGRATION_GUIDE.md](INTEGRATION_GUIDE.md) |
| C5 scan loop, meter, timing budget, console | [SCAN_ENGINE.md](SCAN_ENGINE.md) |
| How the radio is driven | [RADIO_INTERNALS.md](RADIO_INTERNALS.md) |
| What was measured about the C5's RF | [RF_RESEARCH.md](RF_RESEARCH.md) |
| FPVGate side: link, lap detection, races, config, UI | [FPVGATE_INTEGRATION.md](FPVGATE_INTEGRATION.md) |
| Tests and hardware checks | [TESTING.md](TESTING.md) |
