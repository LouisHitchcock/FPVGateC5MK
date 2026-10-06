# Link protocol

The UART protocol between the C5 and its host. This is the byte-level
reference; [HOST_IMPLEMENTATION.md](HOST_IMPLEMENTATION.md) explains how to
build a host around it, and `host/c5host` implements it.

UART, 921600 baud, 8N1, no flow control, 3.3 V. C5: UART1, pins per board
(`NODE_UART_RX_PIN`/`NODE_UART_TX_PIN`, default RX GPIO4 / TX GPIO5). Source
of truth: `multipilot/src/node_core.h`.

Two kinds of traffic share the wire: checksummed text lines (both ways) and
binary scan records (C5 to host, firmware 2 and later).

## Text lines

```text
<payload>*<HH>\n        HH = upper-case hex XOR of the payload bytes
```

`\r` before `\n` is accepted. The C5 rejects and counts lines longer than 96
bytes, bad checksums and lower-case hex (`node` console: `lines ok/bad/long`).

### Host to C5

| Command | Meaning |
|---|---|
| `P,<MHz>,<MHz>,...` | **Scan mode** (firmware 2): one entry per slot, up to 8, `0` = slot off. All or nothing: any bad entry rejects the list and the old one stays. All entries 0 = no frequency. |
| `G,<gain>` | Forced receive gain index, 0 to the C5's limit (89 on chip v1.0). Higher is rejected and the old gain kept. Applies in both modes and is saved across reboots. |
| `Q` | Status request. |
| `F,<MHz>` | Single-frequency mode on one frequency (the firmware 1 protocol). Leaves scan mode. 5180-5917 or the C5 reports `ERR_FREQ`. |

### C5 to host

| Line | Meaning |
|---|---|
| `S,<MHz>,<gain>,<state>,<fw>` | Status: after every F/G/P/Q, and a heartbeat every 900 ms. In scan mode MHz is 0 and state is `SCAN`. |
| `R,<seq u8>,<rssi 0-1023>` | Single-frequency mode only: one reading, ~1 kHz. |

States: `SCAN` (scanning the `P` list), `ERR_FREQ` (no or invalid frequency;
also at boot), `ERR_RF` (radio failed at boot, or repeated failed captures:
5 in a row in single-frequency mode, 40 in scan mode), and `TUNING`/`OK`
(single-frequency mode). Firmware `1` = single-frequency protocol only,
`2` = scan mode.

## Scan records (firmware 2)

```text
0xA5, len, payload[len], crc8
crc8: polynomial 0x07, initial value 0, over len and the payload

payload:
  'M'                 record type
  seq      u8         cycle counter (wraps)
  t0       u32 LE     C5 esp_timer time (us) at the middle of the first slot's capture
  mask     u8         bit i = slot i is in this record
  per slot in the mask, in slot order:
    value  u16 LE     bits 0-9 RSSI 0..1023, bit 15 = this slot's capture failed
    dt     u16 LE     us after t0 (the middle of this slot's capture)
```

8 slots: 7 + 32 = 39-byte payload, 42 bytes on the wire. At ~1030 cycles/s
that is ~43 KB/s, about 47% of the link. A worked example is in
HOST_IMPLEMENTATION.md section 5.

`0xA5` never appears in a text line, so outside a record it always starts
one. Inside a record (payload bytes can be `0xA5`) the parser counts bytes.
The C5 writes each line or record with one `uart_write_bytes` from one task,
so they never interleave.

A command arriving at the C5 while it scans: if bytes are waiting at the
start of a cycle, the cycle is skipped (`aborted` counter) so the command is
handled first; one arriving mid-cycle waits for the end (~1 ms).

## RSSI scale

The C5 maps band power in dB to 0..1023: -10 dB is 0, 53.75 dB is 1023, so
**16 counts per dB** (`Core::setMap`, console `node map lo hi`). The noise
floor at gain 30 is about -3 dB, which reads about 116. FPVGate stores
thresholds on the old 0..255 RX5808 scale, compares against
`threshold x 4`, and its web UI shows `value / 4`.

## Host behaviour

What a host must do, as FPVGate's `C5Link` and `host/c5host` do it:

- **Mode detection.** Send `Q` at start. A status with firmware 2 or more
  means scan mode: send `P` with all 8 slots. (FPVGate falls back to an
  `F`/`OK` slot cycle for firmware 1.)
- **Keeping the list in force.** Resend `P` when the frequencies change, and
  every 250 ms while the C5's state isn't `SCAN` and a slot is enabled (the
  C5 rebooted, or `P` was lost).
- **Stale records.** After sending `P`, drop records until the C5's `SCAN`
  status arrives, and drop any record whose mask doesn't match the slots
  sent.
- **Gain.** Send a changed gain as `G` straight away.
- **Clock mapping.** For each record, `candidate = now_at_parse - t0`. The
  offset is the smallest candidate seen: a lower one replaces it at once;
  otherwise it creeps up 1 us every 16 records (~60 us/s), faster than any
  crystal drift, so it follows drift both ways. Sample time =
  `t0 + dt + offset`. Reset when `P` is sent, and after 8 records in a row
  arrive more than 20 ms late (the C5 clock jumped).
- **Counters** worth keeping: sequence gaps, bad records (length or CRC),
  records accepted, and any queue overflows on the host side.
- **Buffers.** A UART receive buffer of 4096 bytes holds ~95 ms of data.
  FPVGate keeps 128 samples per slot between the link task and lap
  detection.
