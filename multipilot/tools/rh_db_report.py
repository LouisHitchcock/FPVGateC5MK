#!/usr/bin/env python3
"""Receiver report from a RotorHazard database, and replay through rh_usb.

    python tools/rh_db_report.py <database.db>
    python tools/rh_db_report.py <database.db> --replay [--rounds 3] [--nodes 0-7]

The report covers every node with a pilot, per round:
  floor/noise  median level between passes and its spread (robust sigma)
  score        (median pass peak - floor) / noise, comparable across receivers
  margin       (weakest pass - 99th percentile between passes) / pass height;
               above 0 means one EnterAt catches every pass and nothing else
  chatter      laps the marshal deleted within 2 s after a kept lap
  leak         how far a node rises when a pilot on another frequency passes,
               as a fraction of its own pass height, by frequency spacing

--replay runs each node's RSSI history (as RH stored it: peaks and nadirs a
few times a second, interpolated to 1 ms) through the C5's RotorHazard USB
endpoint, test/rh_replay.cpp, with the thresholds the marshal saved, and
scores its laps against the marshalled ones. Build the replay first:
    g++ -std=c++14 -O1 -I src src/rh_usb.cpp test/rh_replay.cpp -o .pio/test/rh_replay.exe

The marshalled laps are taken as the truth. RH's stored history is much
coarser than the C5's 1 kHz samples, so replay results show the direction of
a change, not exact counts.
"""
import argparse
import json
import sqlite3
import statistics as st
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

PASS_WIN = 1.0   # s either side of a kept lap counted as the pass
BG_GUARD = 2.5   # s either side of a kept lap left out of the background
CHATTER = 2.0    # s after a kept lap in which a deleted lap counts as chatter
MATCH = 1.0      # s between a replayed and a marshalled lap to count as the same


def pct(xs, p):
    xs = sorted(xs)
    k = (len(xs) - 1) * p / 100
    f = int(k)
    return xs[f] + (xs[min(f + 1, len(xs) - 1)] - xs[f]) * (k - f)


def parse_range(text):
    out = set()
    for part in text.split(","):
        a, _, b = part.partition("-")
        out.update(range(int(a), int(b or a) + 1))
    return out


def load(db, rounds, nodes):
    c = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    races = {rid: (rnd, heat, start) for rid, rnd, heat, start in
             c.execute("select id, round_id, heat_id, start_time from saved_race_meta")}
    pilots = dict(c.execute("select id, callsign from pilot"))
    rows = []
    for prid, rid, node, pid, hv, ht, enter, exit_, freq in c.execute(
            "select id, race_id, node_index, pilot_id, history_values, history_times, enter_at, exit_at, frequency "
            "from saved_pilot_race"):
        rnd, heat, start = races[rid]
        if not pid or not hv or (rounds and rnd not in rounds) or (nodes and node not in nodes):
            continue
        laps = list(c.execute("select lap_time_stamp / 1000.0, deleted from saved_race_lap "
                              "where pilotrace_id=? order by lap_time_stamp", (prid,)))
        rows.append(dict(race=rid, round=rnd, heat=heat, node=node, pilot=pilots.get(pid, "?"), freq=freq,
                         enter=enter, exit=exit_, times=[t - start for t in json.loads(ht)], vals=json.loads(hv),
                         kept=[t for t, d in laps if not d], deleted=[t for t, d in laps if d]))
    return rows


def signal(r):
    pts = list(zip(r["times"], r["vals"]))
    peaks = [max((v for t, v in pts if abs(t - p) <= PASS_WIN), default=None) for p in r["kept"]]
    peaks = [p for p in peaks if p is not None]
    bg = [v for t, v in pts if t >= 0 and all(abs(t - p) > BG_GUARD for p in r["kept"])]
    if not peaks or len(bg) < 10:
        return None
    floor = st.median(bg)
    noise = max((pct(bg, 84) - pct(bg, 16)) / 2, 1e-9)
    height = max(st.median(peaks) - floor, 1e-9)
    return dict(floor=floor, noise=noise, peak=st.median(peaks), score=(st.median(peaks) - floor) / noise,
                margin=(min(peaks) - pct(bg, 99)) / height)


def report(rows):
    print(f"{'rnd':>3}{'heat':>5}{'node':>5} {'pilot':<14}{'freq':>5}{'enter/exit':>11}{'laps':>5}{'del':>4}"
          f"{'chat':>5}{'floor':>7}{'noise':>6}{'peak':>6}{'score':>7}{'margin':>8}")
    by_round = defaultdict(list)
    for r in sorted(rows, key=lambda r: (r["round"], r["heat"], r["node"])):
        s = signal(r) if len(r["kept"]) >= 2 else None
        chatter = sum(1 for d in r["deleted"] if any(0 < d - k <= CHATTER for k in r["kept"]))
        r.update(sig=s, chatter=chatter)
        by_round[r["round"]].append(r)
        line = (f"{r['round']:>3}{r['heat']:>5}{r['node']:>5} {r['pilot']:<14}{r['freq']:>5}"
                f"{str(r['enter']) + '/' + str(r['exit']):>11}{len(r['kept']):>5}{len(r['deleted']):>4}{chatter:>5}")
        if s:
            line += f"{s['floor']:>7.0f}{s['noise']:>6.1f}{s['peak']:>6.0f}{s['score']:>7.1f}{s['margin']:>8.2f}"
        print(line)
    print("\nPer round (nodes with at least 2 laps):")
    for rnd, rs in sorted(by_round.items()):
        ok = [r for r in rs if r["sig"]]
        if not ok:
            continue
        print(f"  round {rnd}: nodes {len(ok)}/{len(rs)}, laps {sum(len(r['kept']) for r in rs)}, "
              f"deleted {sum(len(r['deleted']) for r in rs)} (chatter {sum(r['chatter'] for r in rs)}), "
              f"median score {st.median(r['sig']['score'] for r in ok):.1f}, "
              f"clean margin {sum(r['sig']['margin'] > 0 for r in ok)}/{len(ok)}")


def leak(rows):
    print("\nLeak from other frequencies (median / p90 of the rise, 1.0 = a node's own pass):")
    by_race = defaultdict(list)
    for r in rows:
        if r["sig"]:
            by_race[r["race"]].append(r)
    out = defaultdict(lambda: defaultdict(list))
    for rs in by_race.values():
        for a in rs:
            for b in rs:
                if b is a or b["freq"] == a["freq"]:
                    continue
                height = max(b["sig"]["peak"] - b["sig"]["floor"], 1e-9)
                for p in a["kept"]:
                    if any(abs(p - q) < BG_GUARD for q in b["kept"]):
                        continue
                    w = [v for t, v in zip(b["times"], b["vals"]) if abs(t - p) <= 0.5]
                    if w:
                        out[a["round"]][abs(a["freq"] - b["freq"])].append((max(w) - b["sig"]["floor"]) / height)
    for rnd in sorted(out):
        cells = [f"{gap} MHz {st.median(x):.2f}/{pct(x, 90):.2f}" for gap, x in sorted(out[rnd].items()) if len(x) >= 5]
        print(f"  round {rnd}: " + ", ".join(cells))


def trace(r):
    """RH's history interpolated to 1 ms, as (ms, level) from shortly before the start."""
    pts = [(t, v) for t, v in zip(r["times"], r["vals"]) if t >= -0.5]
    out = []
    for (t0, v0), (t1, v1) in zip(pts, pts[1:]):
        a, b = int(round((t0 + 0.5) * 1000)), int(round((t1 + 0.5) * 1000))
        for ms in range(a, b):
            out.append((ms + 200, v0 + (v1 - v0) * (ms - a) / max(b - a, 1)))
    return out


def replay(exe, r, merge, smooth):
    # The C5 suspends detection while EnterAt <= ExitAt; RH's marshal allows
    # equal values, so those replay with ExitAt one under EnterAt.
    lines = [f"enter {r['enter']} exit {min(r['exit'], r['enter'] - 1)} merge {merge} smooth {smooth}"]
    lines += [f"{ms} {max(1, min(254, int(round(v))))}" for ms, v in trace(r)]
    res = subprocess.run([str(exe)], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True)
    return [(int(l.split()[1]) - 200) / 1000 - 0.5 for l in res.stdout.split("\n") if l.startswith("lap ")]


def score(laps, truth):
    used, hits = set(), 0
    for p in truth:
        best = min((i for i, t in enumerate(laps) if i not in used and abs(t - p) <= MATCH),
                   key=lambda i: abs(laps[i] - p), default=None)
        if best is not None:
            used.add(best)
            hits += 1
    return len(truth) - hits, len(laps) - len(used)


def replays(rows, exe, settings):
    print(f"\nReplay through rh_usb ({exe.name}), marshal's thresholds, marshalled laps as truth:")
    print(f"  {'merge ms':>8} {'smooth ms':>9} {'nodes':>6} {'passes':>7} {'found':>6} {'missed':>7} {'extra':>6}")
    rs = [r for r in rows if len(r["kept"]) >= 2 and r["enter"] > 1]
    for merge, smooth in settings:
        missed = extra = passes = 0
        for r in rs:
            m, e = score(replay(exe, r, merge, smooth), r["kept"])
            missed += m
            extra += e
            passes += len(r["kept"])
        print(f"  {merge:>8} {smooth:>9} {len(rs):>6} {passes:>7} {passes - missed:>6} {missed:>7} {extra:>6}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("db")
    ap.add_argument("--rounds", type=parse_range, help="e.g. 3 or 1-2")
    ap.add_argument("--nodes", type=parse_range, help="node indexes, e.g. 0-7")
    ap.add_argument("--replay", action="store_true", help="also replay through test/rh_replay.cpp")
    ap.add_argument("--exe", type=Path, default=Path(__file__).resolve().parents[1] / ".pio/test/rh_replay.exe")
    ap.add_argument("--settings", default="0:0,300:0,300:10,300:20",
                    help="merge:smooth pairs to replay (default %(default)s)")
    a = ap.parse_args()
    rows = load(a.db, a.rounds, a.nodes)
    if not rows:
        sys.exit("no nodes with a pilot match")
    report(rows)
    leak(rows)
    if a.replay:
        if not a.exe.exists():
            sys.exit(f"{a.exe} not found; build it (see --help)")
        settings = [tuple(int(x) for x in s.split(":")) for s in a.settings.split(",")]
        replays(rows, a.exe, settings)


if __name__ == "__main__":
    main()
