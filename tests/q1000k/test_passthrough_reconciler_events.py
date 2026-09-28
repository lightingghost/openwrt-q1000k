#!/usr/bin/env python3
"""Exercise the IPv4 reconciler's real ash signal-only loop."""

import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile
import time
import unittest


IPV4 = (Path(__file__).resolve().parents[2] /
        'package/network/utils/q1000k-passthrough/files/ipv4')


class ReconcilerEvents(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='q1000k-pt-events-')
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.events_file = root / 'events'
        self.down_file = root / 'wan-down'
        self.lock_file = root / 'owner.lock'
        source = IPV4.read_text().replace('(exec 9>&-; exec sleep 86400) & timer=$!',
                                          '(exec 9>&-; exec sleep 0.3) & timer=$!')
        self.assertIn('(exec 9>&-; exec sleep 0.3) & timer=$!', source)
        script = root / 'ipv4'
        script.write_text(source)
        command = f'''
export Q1000K_PT_FUNCTIONS_ONLY=1
. {shlex.quote(str(script))}
exec 9>{shlex.quote(str(self.lock_file))}
flock -n 9
reconcile() {{
    printf 'reconcile\\n' >> {shlex.quote(str(self.events_file))}
    if [ -f {shlex.quote(str(self.down_file))} ]; then address=; else address=198.51.100.10; fi
}}
trap 'wake=1' USR1
trap 'exit 0' TERM
run_loop
'''
        self.proc = subprocess.Popen(['busybox', 'ash', '-c', command],
                                     start_new_session=True,
                                     stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE,
                                     text=True)
        self.addCleanup(self.stop)

    def stop(self):
        try:
            os.killpg(self.proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            self.proc.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(self.proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.proc.communicate(timeout=2)

    def events(self):
        return self.events_file.read_text().splitlines() if self.events_file.exists() else []

    def await_count(self, name, count, timeout=1):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.events().count(name) >= count:
                return
            if self.proc.poll() is not None:
                self.fail(f'reconciler exited early: {self.proc.stderr.read()}')
            time.sleep(0.01)
        self.fail(f'timed out waiting for {count} {name} events: {self.events()}')

    def test_wan_events_wake_reconciler_and_elapsed_sleep_does_not(self):
        self.await_count('reconcile', 1)
        time.sleep(0.75)
        self.assertEqual(self.events().count('reconcile'), 1)

        os.kill(self.proc.pid, signal.SIGUSR1)
        self.await_count('reconcile', 2, timeout=0.4)

        self.down_file.touch()
        os.kill(self.proc.pid, signal.SIGUSR1)
        self.await_count('reconcile', 3, timeout=0.4)
        # Even after two sleeper expirations, no unprompted WAN work occurs.
        self.down_file.unlink()
        time.sleep(0.7)
        self.assertEqual(self.events().count('reconcile'), 3)
        os.kill(self.proc.pid, signal.SIGUSR1)
        self.await_count('reconcile', 4, timeout=0.4)

    def test_killing_owner_releases_lock_while_sleeper_is_running(self):
        self.await_count('reconcile', 1)
        time.sleep(0.05)
        os.kill(self.proc.pid, signal.SIGKILL)
        self.proc.wait(timeout=1)
        self.assertEqual(subprocess.run(['flock', '-n', str(self.lock_file), 'true'],
                                        capture_output=True).returncode, 0)


if __name__ == '__main__':
    unittest.main()
