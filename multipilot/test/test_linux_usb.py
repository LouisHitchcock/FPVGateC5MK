"""Real Linux PTYs + pySerial + stock RH discovery, no simulated Serial object.

Run through run_linux_usb.sh. Device parser/protocol are compiled C++ sources;
the radio, USB driver and node.cpp ownership glue are simulated by the bridge.
PTYs do not model USB enumeration, CDC control lines or Pi hardware.
"""
import os
from pathlib import Path
import pty
import subprocess
import sys
import termios
import time
import tty
import unittest
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[1]
RH = Path(os.environ['RH_SOURCE'])
sys.path[:0] = [str(RH / 'src/interface'), str(RH / 'src/server/util')]
import serial
import gevent.lock
import serial_node


class LinuxUsbTest(unittest.TestCase):
    def start_device(self, baseline=False, delay_ms=0):
        master, slave = pty.openpty()
        self.master = master
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        tty.setraw(slave)
        self.path = os.ttyname(slave)
        executable = 'linux_usb_baseline' if baseline else 'linux_usb_fixed'
        process = subprocess.Popen(
            [str(ROOT / '.pio/test/linux' / executable), str(master), str(delay_ms)],
            pass_fds=(master,), stdout=subprocess.PIPE, text=True)
        def stop():
            process.terminate()
            process.wait(timeout=5)
            process.stdout.close()
        self.addCleanup(stop)
        self.assertEqual(process.stdout.readline().strip(), 'ready')

    def inject(self, data):
        # A separate real port owner sends bytes then closes, before RH opens.
        with serial.Serial(self.path, 115200, timeout=0.25) as port:
            port.write(data)
            port.flush()
            time.sleep(0.05)

    def discover(self, expected=8):
        config = Mock()
        config.get_item.return_value = [self.path]
        # Keep RH's real delays, retry loop, reads, checksums and Serial class.
        nodes = serial_node.discover(0, config)
        try:
            self.assertEqual(len(nodes), expected)
            if nodes:
                self.assertEqual([n.multi_node_slot_index for n in nodes], list(range(8)))
                self.assertTrue(all(n.api_level == 36 for n in nodes))
                self.assertEqual(nodes[0].firmware_version_str, 'pty-test')
                for i, node in enumerate(nodes):
                    frequency = node.read_block(None, 0x03, 2)
                    self.assertEqual(int.from_bytes(frequency, 'big'),
                                     [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917][i])
        finally:
            # RH instances share one port for eight seats. Leave the device alive.
            for port in {id(n.serial): n.serial for n in nodes}.values():
                port.close()

    def test_clean_start(self):
        self.start_device()
        self.discover()

    def test_partial_probe_then_three_discovery_restarts(self):
        self.start_device()
        self.inject(b'AT')
        for _ in range(3):
            self.discover()

    def test_overflow_then_discovery(self):
        self.start_device()
        self.inject(b'x' * 200)
        self.discover()

    def test_fragmented_slow_replies(self):
        self.start_device(delay_ms=2)
        self.inject(b'unfinished')
        self.discover()

    def test_linux_tty_echo_before_discovery(self):
        self.start_device()
        # Reproduce a tty echoing device output into its input before RH opens.
        fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY)
        try:
            attrs = termios.tcgetattr(fd)
            attrs[3] |= termios.ECHO
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            # Write from the device end and let the Linux tty echo it back.
            os.write(self.master, b'boot status')
            time.sleep(0.05)
        finally:
            os.close(fd)
        self.discover()

    def test_original_parser_fails_across_restart_then_newline_recovers(self):
        self.start_device(baseline=True)
        self.inject(b'AT')
        for _ in range(2):
            self.discover(expected=0)
        self.inject(b'\n')
        self.discover()


if __name__ == '__main__':
    unittest.main(verbosity=2)
