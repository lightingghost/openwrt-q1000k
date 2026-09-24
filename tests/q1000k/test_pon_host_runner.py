#!/usr/bin/env python3
"""Verify parallel regression coverage, unittest outcomes and cancellation."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

RUNNER = Path(__file__).with_name('run_host_tests.py')


class HostRunnerTests(unittest.TestCase):
    def fixture(self, source):
        temporary = tempfile.TemporaryDirectory(prefix='q1000k-runner-test-')
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / 'test_fixture.py').write_text(source)
        return root

    def command(self, root, jobs=3):
        return [sys.executable, str(RUNNER), '-s', str(root), '-p', 'test_*.py',
                '-j', str(jobs), '--results', str(root / 'results.json')]

    def run_fixture(self, source, jobs=3):
        root = self.fixture(source)
        completed = subprocess.run(self.command(root, jobs), capture_output=True, text=True, timeout=20)
        return root, completed

    def test_every_case_once_with_class_and_module_cleanup(self):
        source = '''import os
from pathlib import Path
import unittest
ROOT = Path(__file__).parent
def record(name):
    (ROOT / name).write_text(str(os.getpid()))
def setUpModule(): record('module-start-' + str(os.getpid()))
def tearDownModule(): record('module-end-' + str(os.getpid()))
class Fixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ready = True
        record('class-start-' + str(os.getpid()))
        cls.addClassCleanup(record, 'class-end-' + str(os.getpid()))
    def check(self):
        self.assertTrue(self.ready)
        with (ROOT / self._testMethodName).open('x') as output:
            output.write(str(os.getpid()))
    test_0 = check
    test_1 = check
    test_2 = check
    test_3 = check
    test_4 = check
'''
        for jobs in (1, 3, 10):
            with self.subTest(jobs=jobs):
                root, completed = self.run_fixture(source, jobs)
                self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
                self.assertTrue(completed.stdout.rstrip().endswith('\nOK'))
                report = json.loads((root / 'results.json').read_text())
                self.assertEqual(report['discovered'], 5)
                self.assertEqual(report['tests_run'], 5)
                self.assertEqual(report['jobs'], min(jobs, 5))
                expected = [f'test_fixture.Fixture.test_{index}' for index in range(5)]
                self.assertCountEqual([name for worker in report['workers'] for name in worker['selected']], expected)
                self.assertCountEqual([row['test'] for worker in report['workers'] for row in worker['timings']], expected)
                pids = {(root / f'test_{index}').read_text() for index in range(5)}
                self.assertEqual(len(pids), min(jobs, 5))
                for pid in pids:
                    for marker in ('module-start', 'module-end', 'class-start', 'class-end'):
                        self.assertTrue((root / f'{marker}-{pid}').exists())

    def test_failures_errors_import_errors_and_missing_results_fail_the_run(self):
        for source, diagnostic in [
            ('def test_it(self): self.fail("assertion sentinel")', 'assertion sentinel'),
            ('def test_it(self): raise RuntimeError("error sentinel")', 'error sentinel'),
            ('@classmethod\n    def setUpClass(cls): raise RuntimeError("setup sentinel")\n'
             '    def test_it(self): pass', 'setup sentinel'),
            ('@unittest.expectedFailure\n    def test_it(self): pass', 'UNEXPECTED SUCCESS'),
            ('def test_it(self): os._exit(0)', 'FAILED'),
        ]:
            with self.subTest(diagnostic=diagnostic):
                _, completed = self.run_fixture('import os, unittest\nclass Fixture(unittest.TestCase):\n    ' + source + '\n')
                self.assertNotEqual(completed.returncode, 0, completed.stdout)
                self.assertIn(diagnostic, completed.stdout + completed.stderr)
                self.assertFalse(completed.stdout.rstrip().endswith('\nOK'))
        _, completed = self.run_fixture('raise ImportError("import sentinel")\n')
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn('import sentinel', completed.stderr)

    def test_skips_and_expected_failures_preserve_unittest_semantics(self):
        root, completed = self.run_fixture('''import unittest
class Fixture(unittest.TestCase):
    @unittest.skip('skip sentinel')
    def test_skip(self): self.fail()
    @unittest.expectedFailure
    def test_expected_failure(self): self.fail()
''')
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        report = json.loads((root / 'results.json').read_text())
        self.assertEqual(report['tests_run'], 2)
        self.assertEqual(sum(worker['skipped'] for worker in report['workers']), 1)
        self.assertEqual(sum(worker['expected_failures'] for worker in report['workers']), 1)

    def test_empty_discovery_and_invalid_job_count_fail(self):
        for source, jobs, diagnostic in [('', 1, 'No tests discovered'), ('', 0, '--jobs must be positive')]:
            with self.subTest(jobs=jobs):
                _, completed = self.run_fixture(source, jobs)
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn(diagnostic, completed.stderr)

    def test_interrupt_stops_workers_and_stubborn_fixture_children(self):
        root = self.fixture('''import os, signal, subprocess, sys, time, unittest
from pathlib import Path
class Fixture(unittest.TestCase):
    def test_wait(self):
        root = Path(__file__).parent
        (root / 'worker.pid').write_text(str(os.getpid()))
        child = "import os,signal,time; from pathlib import Path; signal.signal(signal.SIGTERM, signal.SIG_IGN); Path('child.pid').write_text(str(os.getpid())); time.sleep(60)"
        subprocess.Popen([sys.executable, '-c', child], cwd=root)
        time.sleep(60)
''')
        process = subprocess.Popen(self.command(root), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        deadline = time.monotonic() + 10
        while not (root / 'child.pid').exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue((root / 'child.pid').exists(), 'Fixture did not start')
        pids = [int((root / name).read_text()) for name in ('worker.pid', 'child.pid')]
        def cleanup():
            for pid in pids:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        self.addCleanup(cleanup)
        process.terminate()
        stdout, stderr = process.communicate(timeout=10)
        self.assertNotEqual(process.returncode, 0, stdout + stderr)
        self.assertFalse(stdout.rstrip().endswith('\nOK'))
        def alive(pid):
            try:
                # An orphan can briefly remain a zombie until init reaps it.
                return Path(f'/proc/{pid}/stat').read_text().split(') ', 1)[1][0] != 'Z'
            except FileNotFoundError:
                return False
        deadline = time.monotonic() + 3
        while any(alive(pid) for pid in pids) and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertFalse(any(alive(pid) for pid in pids), 'A worker or fixture survived cancellation')


if __name__ == '__main__':
    unittest.main()
