# Implementing the host side

How to write the firmware on the other end of the C5's UART: the device
that receives the readings and turns them into laps. FPVGate's ESP32-S3
firmware is one such host; this guide is for building your own, on an
ESP32-S3 or any other microcontroller or computer.

There are two ways to do it:

1. **Use the reference library** in [`host/c5host`](../host/c5host): two
   files of portable C++ with no dependencies. It handles the whole protocol
   and is tested against the C5 firmware's own protocol code. Most hosts
   should start here.
2. **Implement the protocol yourself**, for another language or an unusual
   platform. Sections 3 to 8 cover everything you need; the byte-level
   reference is [LINK_PROTOCOL.md](LINK_PROTOCOL.md).

## 1. What the host has to do

| Job | Detail |
|---|---|
| Open the UART | 921600 baud, 8N1, 3.3 V, no flow control |
| Start the C5 | Ask its status, send the list of up to 8 frequencies, set the gain |
| Keep it scanning | Resend the list if the C5 reboots or misses it |
| Read ~43 KB/s without loss | Bulk reads, a 4 KB receive buffer, service the UART every few ms |
| Decode records | Checksums, CRC-8, one reading per pilot per ~1 ms |
| Put timestamps on its own clock | Map the C5's microsecond clock to the host's |
| Detect gate passes | Smoothing, enter/exit thresholds, peak timing, per pilot |

Everything after that (races, UI, storage, announcements) is up to your
application.

### Host hardware requirements

- A hardware UART that runs at 921600 baud with a receive buffer of at least
  4 KB (or DMA). At 8 pilots the C5 sends ~43 KB/s, so a 4 KB buffer covers
  ~95 ms of host stalls.
- A little CPU: parsing is light when the UART is read in bulk. Reading one
  byte per driver call is the main trap: on Arduino-ESP32 that costs ~20 us a
  byte and uses most of a core.
- A microsecond clock for timestamps (`micros()`, `esp_timer_get_time()`,
  `time_us_32()`, `clock_gettime`).

## 2. Using the reference library

Copy `host/c5host/c5host.h` and `c5host.cpp` into your project (or point
PlatformIO at the folder; it has a `library.json`). A complete ESP32-S3
program is in [`host/examples/esp32s3`](../host/examples/esp32s3); the
essentials are:

```cpp
#include "c5host.h"

void writeToC5(void*, const uint8_t* data, size_t len) { Serial1.write(data, len); }

void onSample(void*, const c5host::Sample& s) {
    // s.slot 0-7, s.rssi 0..1023, s.timeUs on your micros() clock.
    // Called from feed(): hand it to your lap logic or a queue, don't block.
}

c5host::Link c5(writeToC5, onSample, nullptr);

void setup() {
    Serial1.setRxBufferSize(4096);   // before begin()
    Serial1.begin(c5host::kBaud, SERIAL_8N1, /*rx*/ 5, /*tx*/ 4);
    const uint16_t raceband[8] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
    c5.setSlots(raceband);   // 0 = slot off
    c5.setGain(30);
    c5.begin(micros());
}

// In a task that runs every ~1 ms:
uint8_t buf[256];
size_t n = Serial1.read(buf, sizeof buf);   // whatever is waiting, in bulk
c5.feed(buf, n, micros());
c5.tick(micros());
```

| API | Use |
|---|---|
| `Link(write, onSample, ctx)` | `write` sends bytes to the C5; `onSample` receives every reading. `ctx` is passed back to both. |
| `setSlots(mhz[8])` | Frequencies in MHz, 0 = off, 5180-5917. Sent on the next `tick()` when changed. Returns false and keeps the old list if an entry is out of range. |
| `setGain(gain)` | 0-89. Sent on the next `tick()` when changed. |
| `begin(nowUs)` | Once, after opening the UART. |
| `feed(data, len, nowUs)` | Every byte received, in order. |
| `tick(nowUs)` | Every few ms. Sends the list, gain and status requests. |
| `online(nowUs)`, `scanning()`, `state()`, `firmware()`, `reportedGain()` | Link state, for your UI |
| `counters()` | `records`, `samples`, `seqGaps`, `badRecords`, `badLines`, `failedDumps`, `staleRecords` |
| `GateDetector` | Per-pilot pass detection (section 7) |

**Threading.** `Link` isn't thread-safe: call `feed()`, `tick()`,
`setSlots()` and `setGain()` from one task. Reading the status accessors
from another task for display is fine. The example runs the link in its own
FreeRTOS task above `loop()`'s priority and passes readings to `loop()`
through a queue.

The PC tests in `multipilot/test/test_host.cpp` connect this library to the
C5 firmware's protocol code through a simulated UART: start-up, readings,
clock mapping, a corrupted record, a C5 reboot, a lost slot list, and gain
and slot changes. Run them with `bash multipilot/test/run.sh`.

## 3. Implementing it yourself: start-up

```text
 host                                   C5
  |  Q*51                          ->    |
  |                                <-    |  S,0,30,ERR_FREQ,2*48   (no list yet; firmware 2)
  |  P,5658,5695,...,5917*51       ->    |
  |  G,30*68                       ->    |
  |                                <-    |  S,0,30,SCAN,2*4D       (list accepted)
  |                                <-    |  record, record, record ... (~1030 a second)
  |                                <-    |  S,0,30,SCAN,2*4D       (heartbeat every 0.9 s)
```

1. Open the UART and send `Q`. The C5 answers with a status line.
2. Check the firmware field (the 5th) is 2 or more. Firmware 1 only has the
   older single-frequency protocol (see LINK_PROTOCOL.md).
3. Send `P` with all eight slots, 0 for unused ones. Send `G` if you want a
   gain other than the one the C5 remembers (it stores the last gain it was
   given).
4. Wait for a status with state `SCAN`. Ignore records until then: any that
   arrive earlier are for the previous list.
5. Read records.

Lines the host sends are the payload, `*`, two upper-case hex digits of the
XOR of the payload bytes, and `\n`:

```c
void sendLine(const char* payload) {
    uint8_t x = 0;
    for (const char* p = payload; *p; ++p) x ^= (uint8_t)*p;
    uartPrintf("%s*%02X\n", payload, x);
}
```

Checked examples: `Q*51`, `G,30*68`, `P,5658,0,5732,0,0,0,0,0*5D`,
`P,5658,5695,5732,5769,5806,5843,5880,5917*51`.

## 4. Receiving: one parser for lines and records

Text lines and binary records share the C5's TX line. Text lines are
printable ASCII ending in `\n`; a record starts with `0xA5`, which never
appears in a line. The C5 writes each line or record in one piece, so they
never interleave. A byte-at-a-time state machine separates them:

```c
enum { TEXT, LENGTH, BODY } st = TEXT;
uint8_t rec[1 + 39];   // length byte, payload (at most 7 + 4 * 8)
int len, pos;

void onByte(uint8_t b) {
    switch (st) {
    case LENGTH:
        if (b < 7 || b > 39) { badRecords++; st = TEXT; break; }
        len = b; rec[0] = b; pos = 0; st = BODY;
        break;
    case BODY:
        if (pos < len) { rec[1 + pos++] = b; break; }
        st = TEXT;   // this byte is the CRC
        if (crc8(rec, len + 1) == b) onRecord(rec + 1, len);
        else badRecords++;
        break;
    case TEXT:
        if (b == 0xA5) { st = LENGTH; lineLen = 0; break; }
        if (b == '\n') { onLine(line, lineLen); lineLen = 0; }
        else if (lineLen < 96) line[lineLen++] = b;
        else lineLen = 0;   // too long: drop it
        break;
    }
}

// CRC-8, polynomial 0x07, initial value 0, over the length byte and payload.
uint8_t crc8(const uint8_t* p, int n) {
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; ++i) c = c & 0x80 ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}
```

For a text line: strip a trailing `\r`, check the last three characters are
`*HH` and that HH matches the XOR of everything before the `*`. Drop lines
that fail. The only line you need in scan mode is the status line,
`S,<MHz>,<gain>,<state>,<firmware>`; its states are `SCAN`, `ERR_FREQ` (no
list), `ERR_RF` (radio failure), and `TUNING`/`OK` (single-frequency mode).

## 5. Decoding a record

```text
payload:
  [0]      'M'
  [1]      seq      u8      +1 per record, wraps
  [2..5]   t0       u32 LE  C5 time (us) at the first slot's reading
  [6]      mask     u8      bit i set: slot i is in this record
  then for each set bit, lowest first:
           value    u16 LE  bits 0-9 RSSI 0..1023; bit 15 set = no reading this cycle
           dt       u16 LE  us after t0 for this slot's reading
```

Check the length is `7 + 4 x (bits set in mask)`, and that the mask equals
the slots you sent; drop the record if not. A gap in `seq` means records
were lost (count it; the next record is still good). Skip values with bit
15 set: the C5's capture failed for that slot this cycle.

Worked example, slots 0 and 4 (5658 and 5806 MHz) in the list:

```text
A5 0F 4D 12 40 42 0F 00 11 F4 01 00 00 2C 81 DC 01 65

A5            record start
0F            length 15 = 7 + 4 x 2
4D            'M'
12            seq 18
40 42 0F 00   t0 = 1000000 us
11            mask: slots 0 and 4
F4 01 00 00   slot 0: value 500, dt 0
2C 81 DC 01   slot 4: value 0x812C, bit 15 set (no reading), dt 476
65            CRC-8 of 0F..01
```

## 6. Staying in sync

| Situation | What to do |
|---|---|
| The status state isn't `SCAN` and you have slots set | Resend `P` every 250 ms until it is. This covers a lost `P`, a C5 reboot (its boot status says `ERR_FREQ`) and a power glitch. |
| You change the slot list | Send `P`, then drop records until the next `SCAN` status, and drop any record whose mask isn't your list's. |
| You change the gain | Send `G`; the C5 confirms with a status line showing the new gain. Gains above the C5's limit are rejected and the status keeps the old one. |
| No status or record for 2.5 s | Treat the C5 as offline. It sends a status every 0.9 s on its own; sending `Q` every second as well finds it quickly after a reconnect. |
| A record fails its CRC | Count it and carry on; the parser is already back in text mode. |

## 7. Timestamps on your clock

Every reading carries the C5's time. Map it onto your own clock so pass
times don't depend on when the bytes happened to arrive:

```text
for each record, at the time you parse it (nowUs on your clock):
    candidate = nowUs - t0
    if no offset yet, or candidate < offset:  offset = candidate   (a faster arrival)
    else every 16th record:                   offset += 1          (creep, tracks drift)
    if 8 records in a row arrive with candidate > offset + 20 ms:
        forget the offset                                          (the C5 rebooted)
sample time = t0 + dt + offset
```

Use 32-bit unsigned arithmetic throughout; both clocks wrap and the
subtraction handles it. Also forget the offset whenever you send `P`.

The smallest delay seen is the best estimate of the true offset, so the
mapping converges on the fastest arrivals within seconds. The creep, ~60 us
per second, is faster than any crystal drift in either direction. Because a
record is sent at the end of its cycle, the mapped times sit about one scan
cycle (~1 ms) later than the true reading time. That shift is the same for
every pilot and every lap, so it cancels out of lap times; subtract the cycle
length if you need absolute times.

## 8. RSSI scale and gate passes

**The scale.** The C5 maps the band power in dB onto 0..1023: -10 dB is 0
and 53.75 dB is 1023, so **16 counts per dB**. At gain 30 the noise floor is
about -3 dB, which reads about 116. A quad at the gate typically reads tens
of dB above the floor. FPVGate stores thresholds on the old RX5808 scale of
0..255 and multiplies them by 4; on your own host use whichever you prefer.

**Gate passes.** The method FPVGate uses, and `c5host::GateDetector`
implements, per pilot:

1. Smooth: `f = (3 f + value) / 4`, on top of the C5's own median-of-3 and
   EMA.
2. Outside the gate, `f >= enter` starts a pass and records `f` and its time
   as the peak.
3. Inside, a higher `f` replaces the peak and its time.
4. `f <= exit` ends the pass. **The pass time is the peak's time.**

`enter` must be above `exit` (hysteresis). Calibrate them per pilot and per
venue: fly a lap, look at the peak and the floor, and set `enter` part way
up from the floor and `exit` a little below it.

**Laps.** On top of the passes, FPVGate applies its race rules: passes
before the race start are ignored, the first pass after the start ends
"Gate 1" (timed from the start), each later pass ends a lap timed from the
previous pass, and passes within the minimum lap time are ignored.

## 9. Checking your host

With 8 Raceband slots and a C5 on the bench:

| Check | Healthy |
|---|---|
| Readings per slot per second | ~1030 |
| `seqGaps`, `badRecords`, queue overflows | 0, or a rare one |
| State | `SCAN` |
| Idle reading, gain 30, no transmitter | ~116 on every slot |
| VTX on one channel | that slot rises by hundreds of counts; the other Raceband slots stay near the floor |
| Power-cycle the C5 | scanning again a few seconds after it boots, with no host restart |

If `seqGaps` rises steadily, the host isn't reading the UART often enough
or in big enough pieces: check the longest gap between reads and the
receive buffer size first.
