"""Run RotorHazard's unchanged USB client against the portable C5MK endpoint.

Requires RH_SOURCE (RH checkout) and the compiled .pio/test/test_rh_usb.exe.
No serial ports, running server or physical devices are used.
"""
import os
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
        self.assertTrue(all(n.api_level == 35 and n.api_valid_flag for n in nodes))
        self.assertEqual((self.serial.dtr, self.serial.rts), (0, 0))

    def test_native_tuning_writes_are_independent_and_can_disable_seats(self):
        self.interface.set_frequency(5, 5800)
        self.assertEqual(self.interface.nodes[5].frequency, 5800)
        self.interface.set_enter_at_level(5, 100)
        self.interface.set_exit_at_level(5, 70)
        self.assertEqual(self.interface.get_value_rssi(self.interface.nodes[5], 0x31), 100)
        self.assertEqual(self.interface.get_value_rssi(self.interface.nodes[5], 0x32), 70)
        self.assertEqual(self.interface.get_value_rssi(self.interface.nodes[6], 0x31), 90)
        self.interface.set_frequency(5, 0)
        self.assertEqual(self.interface.nodes[5].frequency, 0)

    def test_native_update_records_peak_time_not_poll_time(self):
        self.interface.pass_record_callback = Mock()
        self.interface.node_crossing_callback = Mock()
        with patch.object(serial_node, 'monotonic', return_value=50):
            self.interface.update()  # establish the initial lap counters
            for now, value in [(1001, 440), (1003, 600), (1005, 600), (1007, 600), (1008, 300)]:
                self.serial.sample(5, value, now)
            self.serial.now = 1010
            self.interface.update()
        callback = self.interface.pass_record_callback
        callback.assert_called_once()
        node, timestamp, source = callback.call_args.args
        self.assertEqual(node.index, 5)
        self.assertEqual(node.pass_peak_rssi, 150)
        self.assertAlmostEqual(timestamp, 49.995)
        self.assertEqual(source, 0)
        self.assertTrue(node.history_times)

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
