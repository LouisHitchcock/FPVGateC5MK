"""Compare timing estimators on full-rate C5_TIMING_TRACE bench captures.

    python timing_estimator_report.py RUN_DIR [RUN_DIR ...] [--output report.json]

Each directory contains trace.bin (<IHH: elapsed us, filtered RSSI, input
RSSI), scenario.json, trace.json, race.json and truth.csv from the bench.
Pass admission uses the saved entry/exit levels and 300 ms merging. No
estimator changes the RSSI reference, admission thresholds or gain. A single
constant clock offset is removed per run/method; this measures scatter,
not absolute timing accuracy or a constant hardware delay.
"""
import argparse
import bisect
import csv
import json
import math
from pathlib import Path
import statistics as stats
import struct


def detect(times, levels, enter, exit_at, merge_ms=300):
    """Gate threshold/merge rules, never bridging stale trace gaps."""
    passes, start, below = [], None, None
    for i, (t, value) in enumerate(zip(times, levels)):
        if i and t - times[i - 1] > 100:
            start = below = None
        if start is None:
            if value >= enter:
                start = i
            continue
        if below is not None:
            if value >= enter:
                below = None
            elif t - below >= merge_ms:
                passes.append((start, i))
                start = below = None
        elif value <= exit_at:
            below = t
    if start is not None and below is not None and times[-1] - below >= merge_ms:
        passes.append((start, len(times) - 1))
    return passes


def peak_time(times, levels, lo, hi, quantize=1):
    peak = max(v // quantize for v in levels[lo:hi + 1])
    where = [i for i in range(lo, hi + 1) if levels[i] // quantize == peak]
    return (times[where[0]] + times[where[-1]]) / 2


def solve3(matrix, rhs):
    a = [list(row) + [v] for row, v in zip(matrix, rhs)]
    for col in range(3):
        pivot = max(range(col, 3), key=lambda row: abs(a[row][col]))
        a[col], a[pivot] = a[pivot], a[col]
        scale = a[col][col]
        if abs(scale) < 1e-12:
            return None
        a[col] = [v / scale for v in a[col]]
        for row in range(3):
            if row != col:
                scale = a[row][col]
                a[row] = [v - scale * p for v, p in zip(a[row], a[col])]
    return [row[3] for row in a]


def quadratic_time(times, levels, center, radius):
    lo, hi = bisect.bisect_left(times, center - radius), bisect.bisect_right(times, center + radius)
    if hi - lo < 5:
        return None
    x = [(t - center) / radius for t in times[lo:hi]]
    sums = [sum(v ** power for v in x) for power in range(5)]
    rhs = [sum((v ** power) * y for v, y in zip(x, levels[lo:hi])) for power in range(3)]
    fit = solve3([[sums[i + j] for j in range(3)] for i in range(3)], rhs)
    if fit is None or fit[2] >= -1e-9:
        return None
    vertex = -fit[1] / (2 * fit[2])
    return center + vertex * radius if abs(vertex) <= 1 else None


def shoulder_interval(times, levels, lo, hi, drop, start_bound=0):
    peak = max(levels[lo:hi + 1])
    target = peak - drop
    # Include the rising shoulder before Enter, if target is below Enter.
    left_bound = max(start_bound, bisect.bisect_left(times, times[lo] - 2000))
    maximum = max(range(lo, hi + 1), key=lambda i: levels[i])
    rising = [i for i in range(left_bound + 1, maximum + 1)
              if levels[i - 1] < target <= levels[i]]
    falling = [i for i in range(maximum + 1, hi + 1)
               if levels[i] < target <= levels[i - 1]]
    if not rising or not falling:
        return None

    def interpolate(i):
        return times[i - 1] + (times[i] - times[i - 1]) * (
            target - levels[i - 1]) / (levels[i] - levels[i - 1])

    return interpolate(rising[0]), interpolate(falling[-1]), target


def centroid_time(times, levels, interval):
    if interval is None:
        return None
    left, right, target = interval
    lo, hi = bisect.bisect_right(times, left), bisect.bisect_left(times, right)
    points = [(left, 0)] + [(t, max(0, v - target)) for t, v in
                              zip(times[lo:hi], levels[lo:hi])] + [(right, 0)]
    mass = moment = 0.0
    for (ta, wa), (tb, wb) in zip(points, points[1:]):
        dt = tb - ta
        area = dt * (wa + wb) / 2
        # Exact first moment of the linearly interpolated weight.
        mass += area
        moment += ta * area + dt * dt * (wa + 2 * wb) / 6
    return moment / mass if mass > 0 else None


def estimates(times, levels, lo, hi, start_bound=0):
    center = peak_time(times, levels, lo, hi, 4)
    result = {'peak_8bit': center, 'peak_10bit': peak_time(times, levels, lo, hi)}
    for radius in (30, 60, 100):
        result[f'quadratic_{radius}ms'] = quadratic_time(times, levels, center, radius)
    for drop in (16, 32, 48, 64):
        interval = shoulder_interval(times, levels, lo, hi, drop, start_bound)
        result[f'shoulder_drop{drop}'] = (interval[0] + interval[1]) / 2 if interval else None
        if drop == 64:
            result['centroid_drop64'] = centroid_time(times, levels, interval)
    return result


def summarize(errors):
    if not errors:
        return {'count': 0}
    offset = stats.mean(errors)
    centered = [v - offset for v in errors]
    absolute = sorted(map(abs, centered))
    return {'count': len(errors), 'alignment_offset_ms': offset,
            'scatter_ms': stats.pstdev(errors), 'worst_ms': max(absolute),
            'p95_ms': absolute[math.ceil(0.95 * len(absolute)) - 1]}


def report(directory):
    directory = Path(directory)
    info = json.loads((directory / 'trace.json').read_text())
    if info['full'] or info['recordBytes'] != 8:
        raise ValueError('Full or unsupported trace')
    raw = (directory / 'trace.bin').read_bytes()
    if len(raw) != info['count'] * 8:
        raise ValueError('Truncated trace')
    records = list(struct.iter_unpack('<IHH', raw))
    times = [row[0] / 1000 for row in records]
    levels = [row[1] for row in records]
    if any(b <= a for a, b in zip(times, times[1:])):
        raise ValueError('Trace time is not strictly increasing')
    max_gap = max(b - a for a, b in zip(times, times[1:]))
    if max_gap > 100:
        raise ValueError('Stale trace gap; do not score interrupted captures')
    snapshot = json.loads((directory / 'race.json').read_text())
    pilot = next(p for p in snapshot['pilots'] if p['node_index'] == info['pilot'])
    passes = detect(times, levels, pilot['enter_at'] * 4, pilot['exit_at'] * 4)
    with (directory / 'truth.csv').open(newline='') as f:
        truth = [float(row['t_ms']) for row in csv.DictReader(f) if row['kind'] == 'pass']
    if len(passes) != len(truth):
        raise ValueError(f'{directory}: {len(passes)} detected passes, {len(truth)} expected; do not score unmatched data')
    errors, per_pass = {}, []
    start_bound = 0
    for (lo, hi), expected in zip(passes, truth):
        estimates_by_name = estimates(times, levels, lo, hi, start_bound)
        start_bound = hi + 1
        per_pass.append({'truth_ms': expected, 'estimates_ms': estimates_by_name,
                         'peak': max(levels[lo:hi + 1])})
        for name, value in estimates_by_name.items():
            errors.setdefault(name, [])
            if value is not None:
                errors[name].append(value - expected)
    return {'directory': str(directory), 'samples': len(records),
            'max_sample_gap_ms': max_gap,
            'enter': pilot['enter_at'], 'exit': pilot['exit_at'],
            'passes': len(passes), 'methods': {name: summarize(v) for name, v in errors.items()},
            'per_pass': per_pass}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directories', nargs='+', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    results = [report(d) for d in args.directories]
    for result in results:
        print(f"\n{result['directory']}: {result['samples']} samples, {result['passes']} passes")
        for name, values in result['methods'].items():
            if values['count'] != result['passes']:
                print(f"  {name:22s} INCOMPLETE ({values['count']})")
            else:
                print(f"  {name:22s} sd {values['scatter_ms']:7.3f} ms  worst {values['worst_ms']:7.3f} ms")
    if args.output:
        args.output.write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
