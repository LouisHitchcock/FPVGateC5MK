#!/usr/bin/env python3
"""Host tool for the FPVGate C5MK firmware, over the C5's native USB port.

  mp.py --port PORT cmd "status" [--wait 2]      send a command, print the reply
  mp.py --port PORT record --secs 15 [--csv f]   scan statistics per pilot
  mp.py --port PORT capture 5870 [--n 4096]      raw capture + spectrum analysis
  mp.py --port PORT console                      interactive

PORT is the C5's USB serial port, for example COM3 or /dev/ttyACM0.

The port is opened with DTR and RTS low so the C5 is not reset.
"""
import argparse
import math
import struct
import sys
import threading
import time

import serial

SYNC = b"\xA5\x5A"
# Same convention as kSpectrumSign in src/meter.h: RF above the LO appears at
# negative baseband frequency.
SPECTRUM_SIGN = -1
FS = 80e6
PILOT_FLAGS = {1: "fail", 2: "gain", 4: "clip", 8: "fallback"}


def crc16x25(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF


class FrameReader:
    def __init__(self):
        self.buf = bytearray()
        self.crc_errors = 0
        self.junk = 0

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(SYNC)
            if i < 0:
                keep = 1 if self.buf.endswith(SYNC[:1]) else 0
                self.junk += len(self.buf) - keep
                del self.buf[: len(self.buf) - keep]
                return out
            if i:
                self.junk += i
                del self.buf[:i]
            if len(self.buf) < 5:
                return out
            length = self.buf[3] | self.buf[4] << 8
            if length > 4096:
                del self.buf[:2]
                continue
            if len(self.buf) < 7 + length:
                return out
            body = bytes(self.buf[2 : 5 + length])
            crc = self.buf[5 + length] | self.buf[6 + length] << 8
            if crc16x25(body) != crc:
                self.crc_errors += 1
                del self.buf[:2]
                continue
            out.append((chr(body[0]), body[3:]))
            del self.buf[: 7 + length]


def parse_r(payload):
    seq, count, flags, drops, cycle = struct.unpack_from("<HBBII", payload, 0)
    pilots = []
    for k in range(count):
        t, raw, filt, mhz, gain, clips, pf, _ = struct.unpack_from("<IhhHBBBB", payload, 12 + 14 * k)
        pilots.append(dict(t=t, raw=raw / 100, filt=filt / 100, mhz=mhz, gain=gain, clips=clips, flags=pf))
    return dict(seq=seq, flags=flags, drops=drops, cycle=cycle, pilots=pilots)


def open_port(name):
    s = serial.Serial()
    s.port = name
    s.baudrate = 115200
    s.timeout = 0.05
    s.dtr = False
    s.rts = False
    s.open()
    return s


def send(s, line):
    s.write((line.strip() + "\n").encode())


def pump(s, fr, secs, on_frame):
    end = time.time() + secs
    while time.time() < end:
        data = s.read(65536)
        if data:
            for f in fr.feed(data):
                on_frame(*f)


def cmd_cmd(s, a):
    fr = FrameReader()
    s.reset_input_buffer()
    send(s, a.text)
    pump(s, fr, a.wait, lambda t, p: t == "T" and sys.stdout.write(p.decode(errors="replace")))


def stats(vals):
    n = len(vals)
    if not n:
        return float("nan"), float("nan")
    m = sum(vals) / n
    sd = math.sqrt(sum((v - m) ** 2 for v in vals) / (n - 1)) if n > 1 else 0.0
    return m, sd


def cmd_record(s, a):
    fr = FrameReader()
    if not a.no_start:
        send(s, "scan on")
    time.sleep(a.skip)
    s.reset_input_buffer()
    recs = []
    texts = []

    def on(t, p):
        if t == "R":
            recs.append((time.time(), parse_r(p)))
        elif t == "T":
            texts.append(p.decode(errors="replace"))

    pump(s, fr, a.secs, on)
    if texts:
        print("".join(texts), end="")
    if not recs:
        print("no R records received")
        return 1
    # Drop the records before the last settings change.
    first = max([i for i, (_, r) in enumerate(recs) if r["flags"] & 1] or [0])
    recs = recs[first:]
    seqs = [r["seq"] for _, r in recs]
    seq_gaps = sum(((b - a_) & 0xFFFF) - 1 for a_, b in zip(seqs, seqs[1:]))
    cycles = [r["cycle"] for _, r in recs]
    span = recs[-1][0] - recs[0][0]
    print(f"{len(recs)} records in {span:.1f} s, seq gaps {seq_gaps}, crc errors {fr.crc_errors}, "
          f"junk bytes {fr.junk}, chip drops {recs[-1][1]['drops'] - recs[0][1]['drops']}")
    print(f"cycle us: avg {sum(cycles) / len(cycles):.0f} max {max(cycles)}")
    npil = len(recs[-1][1]["pilots"])
    print(" MHz   upd/s  maxgap_ms  raw mean/sd    filt mean/sd   clips fails flags")
    rows = []
    for k in range(npil):
        ps = [r["pilots"][k] for _, r in recs if len(r["pilots"]) > k]
        ts = [p["t"] for p in ps]
        gaps = [((b - a_) & 0xFFFFFFFF) / 1000 for a_, b in zip(ts, ts[1:])]
        dt = ((ts[-1] - ts[0]) & 0xFFFFFFFF) / 1e6 if len(ts) > 1 else 0
        ok = [p for p in ps if not p["flags"] & 1]
        # Skip the filter's first 10 values when judging its sd.
        rm, rsd = stats([p["raw"] for p in ok])
        fm, fsd = stats([p["filt"] for p in ok[10:]])
        flags = 0
        for p in ps:
            flags |= p["flags"]
        fl = ",".join(v for b, v in PILOT_FLAGS.items() if flags & b) or "-"
        print(f"{ps[0]['mhz']:5d} {(len(ts) - 1) / dt if dt else 0:7.1f} {max(gaps) if gaps else 0:9.2f}  "
              f"{rm:6.2f} /{rsd:5.2f}  {fm:6.2f} /{fsd:5.2f}  {sum(p['clips'] for p in ps):5d} "
              f"{len(ps) - len(ok):5d} {fl}")
        rows.append(ps)
    if a.csv:
        with open(a.csv, "w") as f:
            f.write("seq,host_t," + ",".join(f"t{k},raw{k},filt{k},flags{k}" for k in range(npil)) + "\n")
            for ht, r in recs:
                f.write(f"{r['seq']},{ht:.4f}," + ",".join(
                    f"{p['t']},{p['raw']},{p['filt']},{p['flags']}" for p in r["pilots"]) + "\n")
        print("wrote", a.csv)
    return 0


def collect_capture(s, fr, secs=3.0):
    chunks = {}
    meta = {}
    texts = []

    def on(t, p):
        if t == "C":
            cid, lo, gain, bw, total, first, count = struct.unpack_from("<HHBBHHH", p, 0)
            meta.update(id=cid, lo=lo, gain=gain, bw=bw, total=total)
            chunks[first] = struct.unpack_from(f"<{count}I", p, 12)
        elif t == "T":
            texts.append(p.decode(errors="replace"))

    pump(s, fr, secs, on)
    words = []
    for k in sorted(chunks):
        words.extend(chunks[k])
    return meta, words, "".join(texts)


def decode(words):
    import numpy as np
    w = np.array(words, dtype=np.uint32)
    i = ((w & 0x3FF) ^ 0x200).astype(np.int32) - 0x200
    q = (((w >> 10) & 0x3FF) ^ 0x200).astype(np.int32) - 0x200
    g = (w >> 20) & 0x7F
    return i, q, g


def analyse(words, meta, tone_offset_mhz=None):
    import numpy as np
    i, q, g = decode(words)
    n = len(i)
    x = (i - i.mean()) + 1j * (q - q.mean())
    seg = 256
    nseg = n // seg
    win = np.hanning(seg)
    spec = np.zeros(seg)
    for k in range(nseg):
        spec += np.abs(np.fft.fft(x[k * seg:(k + 1) * seg] * win)) ** 2
    spec /= nseg * (win.sum() ** 2)
    spec = np.fft.fftshift(spec)
    f = np.fft.fftshift(np.fft.fftfreq(seg, 1 / FS)) / 1e6
    db = 10 * np.log10(spec + 1e-12)
    print(f"capture id {meta.get('id')}: LO {meta.get('lo')} MHz, n {n}, gain field {g.min()}..{g.max()} "
          f"(forced {meta.get('gain')}), bw {meta.get('bw')}")
    print(f"  DC I {i.mean():.2f} Q {q.mean():.2f} LSB; rms I {i.std():.2f} Q {q.std():.2f}; "
          f"clips {int(((i >= 511) | (i <= -512) | (q >= 511) | (q <= -512)).sum())}")
    pk = int(np.argmax(np.where(np.abs(f) > 1.0, db, -99)))
    print(f"  strongest bin (|f|>1 MHz): {f[pk]:+.2f} MHz at {db[pk]:.1f} dB")
    if tone_offset_mhz is not None:
        # The VTX sits tone_offset above the LO, so it should appear at
        # SPECTRUM_SIGN * offset; its mirror is the opposite bin.
        exp = SPECTRUM_SIGN * tone_offset_mhz

        def level(fc):
            sel = np.abs(f - fc) <= 1.0
            return 10 * np.log10(spec[sel].sum() + 1e-12)

        sig, mir = level(exp), level(-exp)
        print(f"  expected tone at {exp:+.1f} MHz: {sig:.1f} dB; mirror {-exp:+.1f} MHz: {mir:.1f} dB; "
              f"image rejection {sig - mir:.1f} dB")
    # Coarse noise profile (for comparing bw 0 and 1).
    edges = np.arange(-40, 41, 5)
    prof = []
    for lo_, hi_ in zip(edges, edges[1:]):
        sel = (f >= lo_) & (f < hi_) & (np.abs(f) > 0.5)
        prof.append(10 * np.log10(spec[sel].mean() + 1e-12) if sel.any() else float("nan"))
    print("  profile (5 MHz bins, -40..+40):", " ".join(f"{v:5.1f}" for v in prof))
    return f, db


def cmd_capture(s, a):
    fr = FrameReader()
    send(s, "scan off")
    time.sleep(0.3)
    s.reset_input_buffer()
    send(s, f"capture {a.mhz} {a.n}")
    meta, words, text = collect_capture(s, fr)
    print(text, end="")
    if len(words) != meta.get("total", -1):
        print(f"got {len(words)} of {meta.get('total')} words")
        return 1
    analyse(words, meta, a.tone)
    if a.save:
        import numpy as np
        np.save(a.save, np.array(words, dtype=np.uint32))
        print("saved", a.save)
    if not a.keep_off:
        send(s, "scan on")
    return 0


def cmd_console(s, a):
    fr = FrameReader()
    stop = False

    def reader():
        while not stop:
            data = s.read(65536)
            for t, p in fr.feed(data) if data else []:
                if t == "T":
                    sys.stdout.write(p.decode(errors="replace"))
                    sys.stdout.flush()

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    try:
        for line in sys.stdin:
            send(s, line)
    except KeyboardInterrupt:
        pass
    stop = True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="the C5's USB serial port, e.g. COM3 or /dev/ttyACM0")
    sub = ap.add_subparsers(dest="what", required=True)
    p = sub.add_parser("cmd")
    p.add_argument("text")
    p.add_argument("--wait", type=float, default=1.5)
    p = sub.add_parser("record")
    p.add_argument("--secs", type=float, default=10)
    p.add_argument("--skip", type=float, default=0.5, help="seconds to discard first")
    p.add_argument("--csv")
    p.add_argument("--no-start", action="store_true")
    p = sub.add_parser("capture")
    p.add_argument("mhz", type=int, help="LO frequency")
    p.add_argument("--n", type=int, default=4096)
    p.add_argument("--tone", type=float, help="a VTX is this many MHz above the LO")
    p.add_argument("--save")
    p.add_argument("--keep-off", action="store_true")
    sub.add_parser("console")
    a = ap.parse_args()
    s = open_port(a.port)
    try:
        return {"cmd": cmd_cmd, "record": cmd_record, "capture": cmd_capture, "console": cmd_console}[a.what](s, a)
    finally:
        s.close()


if __name__ == "__main__":
    sys.exit(main() or 0)
