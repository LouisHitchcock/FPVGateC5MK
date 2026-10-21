"""Run RotorHazard's unchanged USB client against the portable C5MK endpoint.

Requires RH_SOURCE (RH checkout) and the compiled .pio/test/test_rh_usb.exe.
No serial ports, running server or physical devices are used.
"""
import math
import os
import random
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
RH = Path(os.environ['RH_SOURCE'])
sys.path[:0] = [str(RH / 'src/interface'), str(RH / 'src/server/util')]
import gevent.lock
import RHInterface
import serial_node


class SimSerial:
    def __init__(self):
        self.process = subprocess.Popen([str(ROOT / '.pio/test/test_rh_usb.exe'), '--bridge'],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.port = self.name = 'c5mk-simulator'
        self.baudrate = 115200
        self.now = 1000
        self.buffer = b''
        self.dtr = self.rts = None

    def request(self, line):
        self.process.stdin.write(line + '\n')
        self.process.stdin.flush()
        reply = self.process.stdout.readline()
        if not reply:
            raise RuntimeError('C5MK simulator exited unexpectedly')
        return bytes.fromhex(reply.strip())

    def write(self, data):
        self.buffer += self.request('feed ' + str(self.now) + ' ' + bytes(data).hex(' '))
        return len(data)

    def read(self, size):
        data, self.buffer = self.buffer[:size], self.buffer[size:]
        return data

    def sample(self, seat, value, now):
        self.now = now
        self.request(f'sample {now} {seat} {value}')

    def flushInput(self):
        self.buffer = b''

    def setDTR(self, value):
        self.dtr = value

    def setRTS(self, value):
        self.rts = value

    def setPort(self, port):
        self.port = self.name = port

    def open(self):
        pass

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=5)
        self.process.stdout.close()


def marshal_recalc(values, times, enter_at, exit_at):
    """RH 4.4's Marshal recalculation (src/server/static/marshal.js), lap times only."""
    laps = []
    crossing = False
    peak = peak_first = peak_last = 0
    for rssi, time in zip(values, times):
        if not crossing and rssi > enter_at:
            crossing = True
        if rssi >= peak:
            peak_last = time
            if rssi > peak:
                peak_first = time
                peak = rssi
        if crossing and rssi < exit_at:
            laps.append((peak_last + peak_first) / 2)
            crossing = False
            peak = 0
    if crossing:
        laps.append((peak_last + peak_first) / 2)
    return laps


class SimNodes(list):
    def discover(self, **kwargs):
        self.extend(serial_node.discover(0, kwargs['config']))


class RotorHazardUsbTest(unittest.TestCase):
    def setUp(self):
        self.serial = SimSerial()
        self.addCleanup(self.serial.close)
        serial_patch = patch.object(serial_node.serial, 'Serial', return_value=self.serial)
        sleep_patch = patch.object(serial_node.gevent, 'sleep')
        serial_patch.start()
        sleep_patch.start()
        self.addCleanup(serial_patch.stop)
        self.addCleanup(sleep_patch.stop)
        self.config = Mock()
        self.config.get_item.return_value = ['c5mk-simulator']
        with patch.object(RHInterface, 'Plugins', return_value=SimNodes()):
            self.interface = RHInterface.RHInterface(config=self.config)

    def test_native_discovery_finds_eight_seats_and_firmware(self):
        nodes = self.interface.nodes
        self.assertEqual(len(nodes), 8)
        self.assertEqual([n.multi_node_slot_index for n in nodes], list(range(8)))
        self.assertEqual([n.frequency for n in nodes], [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917])
        self.assertEqual(nodes[0].firmware_version_str, 'usb-dev')
        # RH's newest node API: no "node firmware update available" notice.
        self.assertTrue(all(n.api_level == 36 and n.api_valid_flag for n in nodes))
        self.assertEqual((self.serial.dtr, self.serial.rts), (0, 0))

    def test_native_tuning_writes_are_independent_and_can_disable_seats(self):
        # API 36: each frequency write is checked with the register test.
        self.assertTrue(self.interface.set_frequency(5, 5800))
        self.assertEqual(self.interface.nodes[5].frequency, 5800)
        self.interface.set_enter_at_level(5, 100)
        self.interface.set_exit_at_level(5, 70)
        self.assertEqual(self.interface.get_value_rssi(self.interface.nodes[5], 0x31), 100)
        self.assertEqual(self.interface.get_value_rssi(self.interface.nodes[5], 0x32), 70)
        self.assertEqual(self.interface.get_value_rssi(self.interface.nodes[6], 0x31), 90)
        # RH turns a seat off by writing 1111; the C5 must take it, not keep scanning.
        self.assertTrue(self.interface.set_frequency(5, 0))
        self.assertEqual(self.interface.nodes[5].frequency, 0)
        self.assertEqual(self.interface.get_value_16(self.interface.nodes[5], 0x03), 1111)

    def test_native_update_records_peak_time_not_poll_time(self):
        self.interface.pass_record_callback = Mock()
        self.interface.node_crossing_callback = Mock()
        with patch.object(serial_node, 'monotonic', return_value=50):
            self.interface.update()  # establish the initial lap counters
            for now, value in [(1001, 440), (1003, 600), (1005, 600), (1007, 600), (1008, 300)]:
                self.serial.sample(5, value, now)
            # The pass ends once the level has stayed under ExitAt for 300 ms.
            self.serial.now = 1010
            self.interface.update()
            self.interface.pass_record_callback.assert_not_called()
            self.serial.sample(5, 300, 1308)
            self.serial.now = 1310
            self.interface.update()
        callback = self.interface.pass_record_callback
        callback.assert_called_once()
        node, timestamp, source = callback.call_args.args
        self.assertEqual(node.index, 5)
        self.assertEqual(node.pass_peak_rssi, 150)
        self.assertAlmostEqual(timestamp, 49.695)  # still the peak (1005), reported 305 ms later
        self.assertEqual(source, 0)
        self.assertTrue(node.history_times)

    def test_native_history_keeps_a_short_peak_between_polls(self):
        # Sensor Tuning's graph is RH's RSSI history. A 3 ms pass between two
        # polls must reach it at its real height, not the level at poll time.
        node = self.interface.nodes[5]
        self.interface.set_enter_at_level(5, 200)  # history only, no crossing
        self.interface.set_exit_at_level(5, 190)
        self.interface.pass_record_callback = Mock()
        self.interface.node_crossing_callback = Mock()
        with patch.object(serial_node, 'monotonic', return_value=50):
            self.serial.sample(5, 120, 1000)
            self.serial.now = 1001
            self.interface.update()
            for now in range(1002, 1050):
                self.serial.sample(5, 440 if 1020 <= now < 1023 else 120, now)
            self.serial.now = 1050
            self.interface.update()
        self.assertIn(110, node.history_values)  # the pass in between, for Marshal
        # The live graph plots current_rssi every ~200 ms: it holds the pass for ~250 ms.
        self.assertEqual(node.current_rssi, 110)
        with patch.object(serial_node, 'monotonic', return_value=50):
            for now in range(1050, 1300):
                self.serial.sample(5, 120, now)
            self.serial.now = 1300
            self.interface.update()
        self.assertEqual(node.current_rssi, 30)  # then the floor again

    def test_marshal_recalc_matches_realtime_laps(self):
        # RH's Marshal recalculates laps from the RSSI history it stored. With
        # the same EnterAt/ExitAt it must land on the same times as the laps
        # recorded live, even when multipath ripples the top of each pass and
        # RH polls slowly or unevenly (it polls every node in turn).
        for seed, interval in [(1, 80), (2, 100), (3, 130), (4, 170), (5, 250)]:
            with self.subTest(seed=seed, poll_ms=interval):
                self.check_recalc(seed, interval)

    def check_recalc(self, seed, interval):
        self.setUp()
        node = self.interface.nodes[5]
        self.interface.set_enter_at_level(5, 140)
        self.interface.set_exit_at_level(5, 110)
        self.interface.pass_record_callback = Mock()
        self.interface.node_crossing_callback = Mock()
        self.interface.race_status = RHInterface.BaseHardwareInterface.RACE_STATUS_RACING
        rng = random.Random(seed)
        passes = [(2000 + 1500 * i, rng.uniform(30, 80), rng.uniform(0, math.tau)) for i in range(10)]

        def level(t):
            value = 75 + rng.gauss(0, 3)
            for centre, period, phase in passes:
                if abs(t - centre) < 600:
                    bump = 115 * math.exp(-0.5 * ((t - centre) / 150) ** 2)
                    value += bump * (1 - 0.25 * abs(math.sin(math.tau * (t - centre) / period + phase)))
            return max(1, min(1023, round(value * 4)))

        with patch.object(serial_node, 'monotonic', side_effect=lambda: self.serial.now / 1000):
            self.serial.now = 1000
            self.interface.update()
            next_poll = 1000 + interval
            for now in range(1001, passes[-1][0] + 1000):
                self.serial.sample(5, level(now), now)
                if now >= next_poll:
                    self.serial.now = now
                    self.interface.update()
                    next_poll = now + interval + rng.randint(-20, 20)
        realtime = [call.args[1] for call in self.interface.pass_record_callback.call_args_list]
        self.assertEqual(len(realtime), len(passes))
        recalc = marshal_recalc(node.history_values, node.history_times, 140, 110)
        self.assertEqual(len(recalc), len(passes))
        offsets = [round((min(recalc, key=lambda x: abs(x - r)) - r) * 1000, 1) for r in realtime]
        self.assertLessEqual(max(map(abs, offsets)), 1.0, f'recalc minus realtime, ms: {offsets}')

    def test_native_capture_uses_live_rssi(self):
        node = self.interface.nodes[5]
        self.interface.pass_record_callback = Mock()
        self.interface.node_crossing_callback = Mock()
        self.interface.new_enter_or_exit_at_callback = Mock()
        self.serial.sample(5, 200, 1000)
        self.assertTrue(self.interface.start_capture_exit_at_level(5))
        node.cap_exit_at_millis = -1  # finish on the next sample, without a real-time wait
        self.interface.update()
        self.assertFalse(node.cap_exit_at_flag)
        self.assertEqual(node.exit_at_level, 50)


if __name__ == '__main__':
    unittest.main()
