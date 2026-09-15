#!/usr/bin/env python3
"""Exercise serial collection through Linux PTYs without device access."""
import importlib.util
import os
from pathlib import Path
import pty
import select
import tempfile
import termios
import time
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('bench_serial', Path(__file__).resolve().parents[2] /
                                             'scripts/q1000k/bench-serial.py')
SERIAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SERIAL)


class SerialTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.output = Path(self.directory.name) / 'serial.log'
        self.master, self.slave = pty.openpty()
        self.device = os.ttyname(self.slave)
        self.original = termios.tcgetattr(self.slave)
        self.addCleanup(self.close_pty)

    def close_pty(self):
        for fd in (self.master, self.slave):
            if fd is not None:
                os.close(fd)

    def wait_for(self, condition):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if condition():
                return
            time.sleep(0.01)
        self.fail('Timed out waiting for serial reader')

    def test_binary_capture_configuration_restore_and_no_transmission(self):
        payload = b'boot\r\n\x00\xff\x11\x13 no translation\r'
        with SERIAL.SerialCapture(self.device, self.output) as capture:
            settings = termios.tcgetattr(self.slave)
            self.assertEqual(settings[0], 0)
            self.assertEqual(settings[1], 0)
            self.assertEqual(settings[3], 0)
            self.assertEqual(settings[4:6], [termios.B115200, termios.B115200])
            self.assertEqual(settings[2] & termios.CSIZE, termios.CS8)
            self.assertFalse(settings[2] & (termios.PARENB | termios.CSTOPB |
                                          getattr(termios, 'CRTSCTS', 0)))
            os.write(self.master, payload)
            self.wait_for(lambda: self.output.read_bytes() == payload)
            capture.check()
            self.assertEqual(select.select([self.master], [], [], 0)[0], [])
            self.assertEqual(self.output.stat().st_mode & 0o777, 0o600)
        self.assertEqual(termios.tcgetattr(self.slave), self.original)
        capture.stop()  # idempotent
        with SERIAL.SerialCapture(self.device, self.output.with_name('again.log')):
            pass

    def test_exception_restores_settings(self):
        with self.assertRaisesRegex(ValueError, 'outer error'):
            with SERIAL.SerialCapture(self.device, self.output):
                raise ValueError('outer error')
        self.assertEqual(termios.tcgetattr(self.slave), self.original)

    def test_interrupted_setup_restores_settings(self):
        real_open = os.open
        def interrupt_log_open(path, *args, **kwargs):
            if path == self.output:
                raise KeyboardInterrupt()
            return real_open(path, *args, **kwargs)
        with patch.object(SERIAL.os, 'open', side_effect=interrupt_log_open):
            with self.assertRaises(KeyboardInterrupt):
                with SERIAL.SerialCapture(self.device, self.output):
                    pass
        self.assertEqual(termios.tcgetattr(self.slave), self.original)
        with SERIAL.SerialCapture(self.device, self.output):
            pass

    def test_context_exit_drains_pending_bytes(self):
        payload = b'last serial message\r\n' * 100
        with SERIAL.SerialCapture(self.device, self.output):
            os.write(self.master, payload)
        self.assertEqual(self.output.read_bytes(), payload)

    def test_second_capture_is_rejected_without_disturbing_first(self):
        with SERIAL.SerialCapture(self.device, self.output) as capture:
            configured = termios.tcgetattr(self.slave)
            with self.assertRaises(SERIAL.SerialCaptureError):
                with SERIAL.SerialCapture(self.device, self.output.with_name('second.log')):
                    self.fail('Concurrent logger admitted')
            self.assertEqual(termios.tcgetattr(self.slave), configured)
            os.write(self.master, b'still recording')
            self.wait_for(lambda: self.output.read_bytes() == b'still recording')
            capture.check()
        self.assertEqual(termios.tcgetattr(self.slave), self.original)

    def test_existing_output_is_preserved_and_setup_is_undone(self):
        self.output.write_bytes(b'preserved')
        with self.assertRaises(SERIAL.SerialCaptureError):
            with SERIAL.SerialCapture(self.device, self.output):
                pass
        self.assertEqual(self.output.read_bytes(), b'preserved')
        self.assertEqual(termios.tcgetattr(self.slave), self.original)
        with SERIAL.SerialCapture(self.device, self.output.with_name('fresh.log')):
            pass

    def test_disconnect_surfaces_reader_error_and_closes_fds(self):
        capture = SERIAL.SerialCapture(self.device, self.output).start()
        os.close(self.master)
        self.master = None
        self.wait_for(lambda: capture._error is not None)
        with self.assertRaises(SERIAL.SerialCaptureError):
            capture.check()
        with self.assertRaises(SERIAL.SerialCaptureError):
            capture.stop()
        self.assertIsNone(capture._fd)
        self.assertIsNone(capture._output)
        self.assertIsNone(capture._thread)

    def test_write_error_surfaces_and_restores(self):
        capture = SERIAL.SerialCapture(self.device, self.output).start()
        real_write = os.write
        def failing_log_write(fd, data):
            if fd == capture._output:
                raise OSError('test disk full')
            return real_write(fd, data)
        with patch.object(SERIAL.os, 'write', side_effect=failing_log_write):
            os.write(self.master, b'log this')
            self.wait_for(lambda: capture._error is not None)
        with self.assertRaisesRegex(SERIAL.SerialCaptureError, 'disk full'):
            capture.stop()
        self.assertEqual(termios.tcgetattr(self.slave), self.original)

    def test_regular_file_cannot_be_serial_input(self):
        fake = self.output.with_name('fake-device')
        fake.write_bytes(b'do not alter')
        with self.assertRaises(SERIAL.SerialCaptureError):
            with SERIAL.SerialCapture(fake, self.output):
                pass
        self.assertEqual(fake.read_bytes(), b'do not alter')
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
