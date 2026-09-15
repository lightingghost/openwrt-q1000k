#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read-only Linux serial logging for the RX collector (standard library only)."""
import copy
import fcntl
import os
from pathlib import Path
import select
import stat
import termios
import threading


class SerialCaptureError(RuntimeError):
    """Serial setup, capture or restoration failed."""


class SerialCapture:
    """Capture 115200 8N1 bytes; restore the port on every exit path.

    Construction has no side effects. Enter the context (or call start()),
    call check() during long operations, and stop() before packaging output.
    The port is opened read-only and no bytes are ever sent to it. Advisory
    locking coordinates collectors; TIOCEXCL prevents new ordinary opens.
    An already-open, uncooperative reader cannot be detected by these locks.
    """

    def __init__(self, device, output_path):
        self.device = os.fspath(device)
        self.output_path = Path(output_path)
        self._fd = self._output = None
        self._original = None
        self._exclusive = False
        self._thread = None
        self._stop = threading.Event()
        self._error = None
        self._started = False

    def _fail(self, operation, error):
        if self._error is None:
            self._error = SerialCaptureError(f'{operation}: {error}')

    def check(self):
        if self._error is not None:
            raise self._error

    def start(self):
        if self._started:
            raise SerialCaptureError('Serial capture cannot be started twice')
        self._started = True
        try:
            self._fd = os.open(self.device, os.O_RDONLY | os.O_NOCTTY |
                               os.O_NONBLOCK | os.O_CLOEXEC)
            if not stat.S_ISCHR(os.fstat(self._fd).st_mode) or not os.isatty(self._fd):
                raise SerialCaptureError('Serial input must be a terminal character device')
            fcntl.flock(self._fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.ioctl(self._fd, termios.TIOCEXCL)
            self._exclusive = True
            self._original = termios.tcgetattr(self._fd)
            configured = copy.deepcopy(self._original)
            configured[0] = 0  # no translations, software flow control or parity processing
            configured[1] = 0
            configured[2] &= ~(termios.CSIZE | termios.PARENB | termios.PARODD |
                               termios.CSTOPB | getattr(termios, 'CRTSCTS', 0) |
                               getattr(termios, 'CMSPAR', 0) | termios.HUPCL)
            configured[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
            configured[3] = 0
            configured[4] = configured[5] = termios.B115200
            configured[6][termios.VMIN] = 1
            configured[6][termios.VTIME] = 0
            termios.tcsetattr(self._fd, termios.TCSANOW, configured)
            # Never truncate or follow a pre-existing log/symlink.
            self._output = os.open(self.output_path, os.O_WRONLY | os.O_CREAT |
                                   os.O_EXCL | os.O_APPEND | os.O_CLOEXEC, 0o600)
            os.fchmod(self._output, 0o600)
            self._thread = threading.Thread(target=self._capture, name='q1000k-serial',
                                            daemon=True)
            self._thread.start()
        except BaseException as error:
            if isinstance(error, Exception):
                self._fail('Cannot start serial capture', error)
                self.stop()
            else:
                try:
                    self.stop()
                except SerialCaptureError as cleanup_error:
                    if hasattr(error, 'add_note'):
                        error.add_note(str(cleanup_error))
                raise
        return self

    def _capture(self):
        try:
            drained = 0
            while True:
                stopping = self._stop.is_set()
                # Drain already buffered bytes on close, bounded even if the
                # device continues talking. Never wait for a new serial byte.
                if stopping and drained >= 16:
                    break
                readable, _, _ = select.select([self._fd], [], [], 0 if stopping else 0.1)
                if not readable:
                    if stopping:
                        break
                    continue
                try:
                    data = os.read(self._fd, 65536)
                except BlockingIOError:
                    continue
                if not data:
                    raise SerialCaptureError('Serial device reached EOF or disconnected')
                view = memoryview(data)
                while view:
                    written = os.write(self._output, view)
                    if written <= 0:
                        raise SerialCaptureError('Serial log write made no progress')
                    view = view[written:]
                if stopping:
                    drained += 1
        except Exception as error:
            self._fail('Serial capture failed', error)

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2)
            if self._thread.is_alive():
                self._fail('Cannot stop serial capture', 'Reader did not stop within two seconds')
                self.check()
            self._thread = None
        if self._fd is not None:
            if self._original is not None:
                try:
                    termios.tcsetattr(self._fd, termios.TCSANOW, self._original)
                except Exception as error:
                    self._fail('Cannot restore serial settings', error)
                self._original = None
            if self._exclusive:
                try:
                    fcntl.ioctl(self._fd, termios.TIOCNXCL)
                except Exception as error:
                    self._fail('Cannot release serial exclusivity', error)
                self._exclusive = False
            try:
                os.close(self._fd)  # also releases the advisory lock
            except OSError as error:
                self._fail('Cannot close serial device', error)
            self._fd = None
        if self._output is not None:
            try:
                os.close(self._output)
            except OSError as error:
                self._fail('Cannot close serial log', error)
            self._output = None
        self.check()

    def __enter__(self):
        return self.start()

    def __exit__(self, exc_type, exc, traceback):
        try:
            self.stop()
        except SerialCaptureError as error:
            if exc is None:
                raise
            if hasattr(exc, 'add_note'):
                exc.add_note(str(error))
        return False
