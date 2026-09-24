#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run every discovered host test in bounded, isolated unittest workers."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


def test_cases(suite):
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            yield from test_cases(test)
        else:
            yield test


class TimedResult(unittest.TextTestResult):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.timings = []

    def startTest(self, test):
        self.started = time.monotonic()
        super().startTest(test)

    def stopTest(self, test):
        self.timings.append(dict(test=test.id(), seconds=time.monotonic() - self.started))
        super().stopTest(test)


def worker(plan, result_path):
    data = json.loads(plan.read_text())
    sys.path.insert(0, data['directory'])
    suite = unittest.defaultTestLoader.loadTestsFromNames(data['tests'])
    result = unittest.TextTestRunner(verbosity=2, resultclass=TimedResult).run(suite)
    result_path.write_text(json.dumps(dict(
        successful=result.wasSuccessful(), tests_run=result.testsRun,
        failures=len(result.failures), errors=len(result.errors),
        skipped=len(result.skipped), expected_failures=len(result.expectedFailures),
        unexpected_successes=len(result.unexpectedSuccesses), timings=result.timings), indent=2) + '\n')
    return 0 if result.wasSuccessful() else 1


def stop_workers(processes):
    # Workers own process groups so cancellation also stops their compilers,
    # shell fixtures and fake services, including children of an exited worker.
    for sig in (signal.SIGTERM, signal.SIGKILL):
        for process in processes:
            try:
                os.killpg(process.pid, sig)
            except ProcessLookupError:
                pass
        if sig == signal.SIGTERM:
            deadline = time.monotonic() + 3
            for process in processes:
                try:
                    process.wait(timeout=max(0, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    pass
    for process in processes:
        process.wait()


def run_tests(directory, pattern, jobs, results=None):
    started = time.monotonic()
    loader = unittest.TestLoader()
    suite = loader.discover(str(directory), pattern=pattern)
    if loader.errors:
        print('\n'.join(loader.errors), file=sys.stderr)
        return 1
    names = [test.id() for test in test_cases(suite)]
    if not names:
        print('No tests discovered; refusing an empty regression run.', file=sys.stderr)
        return 1
    jobs = min(jobs, len(names))
    # Stripe individual cases, not modules: activation scenarios dominate the
    # runtime and otherwise all land in a single worker. Keep discovery order
    # inside each worker so unittest retains its class/module fixture lifecycle.
    shards = [names[index::jobs] for index in range(jobs)]
    print(f'Running {len(names)} tests with {jobs} workers', flush=True)
    reports = []
    processes = []
    with tempfile.TemporaryDirectory(prefix='q1000k-host-tests-') as temporary:
        root = Path(temporary)
        pending = {}
        try:
            for index, tests in enumerate(shards):
                plan, result_path, log = [root / f'{index}.{suffix}' for suffix in ('plan', 'json', 'log')]
                plan.write_text(json.dumps(dict(directory=str(directory), tests=tests)))
                with log.open('wb') as output:
                    process = subprocess.Popen(
                        [sys.executable, '-u', str(Path(__file__).resolve()),
                         '--worker', str(plan), '--worker-result', str(result_path)],
                        stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
                processes.append(process)
                pending[index] = (process, result_path, log, time.monotonic())
            while pending:
                for index, (process, result_path, log, launched) in list(pending.items()):
                    if process.poll() is None:
                        continue
                    print(f'\n--- Worker {index + 1}/{jobs} ---', flush=True)
                    print(log.read_text(errors='replace'), end='', flush=True)
                    report = json.loads(result_path.read_text()) if result_path.exists() else {}
                    report.update(worker=index + 1, selected=shards[index],
                                  seconds=time.monotonic() - launched, returncode=process.returncode)
                    if process.returncode or not report.get('successful'):
                        stop_workers([process])
                    reports.append(report)
                    del pending[index]
                if pending:
                    time.sleep(0.05)
        except BaseException:
            stop_workers(processes)
            for index, (_, _, log, _) in pending.items():
                print(f'\n--- Interrupted worker {index + 1}/{jobs} ---', flush=True)
                print(log.read_text(errors='replace'), end='', flush=True)
            raise
    successful = all(report.get('successful') and report['returncode'] == 0 for report in reports)
    summary = dict(successful=successful, discovered=len(names), jobs=jobs,
                   tests_run=sum(report.get('tests_run', 0) for report in reports),
                   seconds=time.monotonic() - started,
                   workers=sorted(reports, key=lambda report: report['worker']))
    if results is not None:
        results.write_text(json.dumps(summary, indent=2) + '\n')
    print(f'\nRan {summary["tests_run"]} tests across {jobs} workers in {summary["seconds"]:.3f}s')
    # Preserve the final unittest status consumed by bench-revalidate.py.
    print('\nOK' if successful else '\nFAILED (see worker output above)', flush=True)
    return 0 if successful else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-s', '--start-directory', type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument('-p', '--pattern', default='test_pon_*.py')
    parser.add_argument('-j', '--jobs', type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument('--results', type=Path, help='Save worker results and per-test timings as JSON')
    parser.add_argument('--worker', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--worker-result', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.worker is not None:
        return worker(args.worker, args.worker_result)
    return run_tests(args.start_directory.resolve(), args.pattern, args.jobs, args.results)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'Interrupted by signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, interrupted)
    raise SystemExit(main())
