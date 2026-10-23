"""Numerical and admission checks for the offline timing comparison tool."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('timing', Path(__file__).resolve().parents[1] /
                                             'tools/timing_estimator_report.py')
timing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(timing)


class TimingEstimatorTest(unittest.TestCase):
    def test_symmetric_pass_and_clock_origin(self):
        times = list(range(2100))
        levels = [max(100, round(800 - 0.002 * (t - 1000) ** 2)) for t in times]
        passes = timing.detect(times, levels, 520, 380)
        self.assertEqual(len(passes), 1)
        estimates = timing.estimates(times, levels, *passes[0])
        shifted = timing.estimates([t + 123456 for t in times], levels, *passes[0])
        for name, value in estimates.items():
            with self.subTest(name=name):
                self.assertIsNotNone(value)
                self.assertAlmostEqual(value, 1000, places=5)
                self.assertAlmostEqual(shifted[name] - value, 123456, places=5)

    def test_quadratic_recovers_fractional_vertex(self):
        times = list(range(200))
        levels = [700 - 0.02 * (t - 103.25) ** 2 for t in times]
        self.assertAlmostEqual(timing.quadratic_time(times, levels, 100, 60), 103.25, places=6)

    def test_triangular_area_centroid(self):
        self.assertAlmostEqual(timing.centroid_time([0, 2, 8], [0, 6, 0], (0, 8, 0)), 10 / 3)

    def test_missing_shoulders_and_flat_fit_are_not_scored(self):
        times, levels = list(range(200)), [700] * 200
        self.assertIsNone(timing.shoulder_interval(times, levels, 0, 199, 64))
        self.assertIsNone(timing.quadratic_time(times, levels, 100, 60))
        self.assertEqual(timing.summarize([]), {'count': 0})

    def test_multipath_merge_and_stale_gap(self):
        times = list(range(2500))
        levels = [700 if 100 <= t < 200 or 300 <= t < 400 or 1500 <= t < 1600 else 100 for t in times]
        self.assertEqual(len(timing.detect(times, levels, 500, 300)), 2)
        self.assertEqual(len(timing.detect(times, levels, 500, 300, merge_ms=0)), 3)
        self.assertEqual(timing.detect([0, 1, 200, 201], [700, 700, 100, 100], 500, 300), [])

    def test_scatter_removes_only_one_constant_offset(self):
        result = timing.summarize([100, 110, 120])
        self.assertEqual(result['alignment_offset_ms'], 110)
        self.assertEqual(result['worst_ms'], 10)
        self.assertAlmostEqual(result['scatter_ms'], (200 / 3) ** 0.5)

    def test_shoulders_do_not_reuse_a_previous_pass(self):
        times = list(range(1600))
        levels = [700 if 100 <= t < 200 or 900 <= t < 1000 else 100 for t in times]
        passes = timing.detect(times, levels, 500, 300)
        self.assertEqual(len(passes), 2)
        result = timing.estimates(times, levels, *passes[1], start_bound=passes[0][1] + 1)
        self.assertAlmostEqual(result['shoulder_drop64'], 949.5)

    def test_invalid_traces_are_rejected(self):
        cases = [
            (True, b'', 'Full or unsupported'),
            (False, b'bad', 'Truncated'),
            (False, struct.pack('<IHHIHH', 1, 500, 500, 1, 600, 600), 'strictly increasing'),
            (False, struct.pack('<IHHIHH', 0, 500, 500, 200000, 600, 600), 'Stale trace gap'),
        ]
        for full, data, error in cases:
            with self.subTest(error=error), tempfile.TemporaryDirectory() as directory:
                path = Path(directory)
                (path / 'trace.json').write_text(json.dumps({'full': full, 'recordBytes': 8, 'count': 2}))
                (path / 'trace.bin').write_bytes(data)
                with self.assertRaisesRegex(ValueError, error):
                    timing.report(path)


if __name__ == '__main__':
    unittest.main()
