#!/usr/bin/env python3
"""Portable collection remains offline, stops on faults and preserves evidence."""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('bench_collect', ROOT/'scripts/q1000k/bench-collect.py')
COLLECT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COLLECT)


def artifact(path):
    path.mkdir()
    image = b'fixture RAM image'
    helper = b'RX_DIAGNOSTICS_VERSION=2\n'
    (path/COLLECT.IMAGE).write_bytes(image)
    (path/'selection.json').write_text(json.dumps(dict(revision='a'*40)))
    (path/'checkpoint.json').write_text(json.dumps(dict(status='passed', revision='a'*40,
                                                        sha256=hashlib.sha256(image).hexdigest())))
    runner = COLLECT.module(ROOT, 'bench-run')
    names = ['/usr/sbin/q1000k-pon-bench', '/lib/q1000k-xgspon/common.sh',
             '/usr/share/libubox/jshn.sh', '/usr/sbin/q1000k-omci', '/usr/libexec/q1000k-omci-config']
    names += ['/lib/modules/6.18.44/' + (name.replace('_', '-') if name == 'q1000k_pon_control' else name)
              + '.ko' for name in runner.MODULES]
    (path/'runtime-sha256sums').write_text(''.join(
        (hashlib.sha256(helper).hexdigest() if name == names[0] else 'b'*64)+'  '+name+'\n' for name in names))
    runtime = path/'runtime/usr/sbin/q1000k-pon-bench'
    runtime.parent.mkdir(parents=True)
    runtime.write_bytes(helper)
    return path


def rows(los, start=0, count=15, irq=0):
    return [dict(controller_los=los, phy_los=los, sampled_ms=(start+n+1)*1000,
                 irq_calls=irq, poll_calls=start+n+1) for n in range(count)]


class PortableTests(unittest.TestCase):
    def test_generated_file_runs_without_repository_or_artifact_access(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            root = Path(directory)
            saved = artifact(root/'artifact')
            script = root/'collector.py'
            COLLECT.build_single_file(script, saved)
            # Make the original artifact unavailable to the generated collector.
            saved.rename(root/'hidden-artifact')
            elsewhere = root/'elsewhere'
            elsewhere.mkdir()
            completed = subprocess.run([sys.executable, str(script), '--dry-run'], cwd=elsewhere,
                                       capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            result = json.loads(completed.stdout)
            self.assertEqual(result['host'], '192.168.255.1')
            self.assertEqual(result['mode'], 'live-control')
            self.assertEqual(result['artifact']['revision'], 'a'*40)
            self.assertEqual(result['artifact']['diagnostics_version'], 2)
            self.assertFalse(result['optical_service_verified'])
            # Importing every companion exercises the embedded header and all
            # reader dependencies, without importing from the original checkout.
            command = ('import importlib.util; '
                       f's=importlib.util.spec_from_file_location("portable", {str(script)!r}); '
                       'm=importlib.util.module_from_spec(s); s.loader.exec_module(m); '
                       'c=m.workspace(); r,a=c.__enter__(); '
                       'x=m.module(r,"bench-control-report"); '
                       'print(len(x.SUITE.REPORT.probe_summary.__name__)); c.__exit__(None,None,None)')
            imported = subprocess.run([sys.executable, '-c', command], cwd=elsewhere, capture_output=True, text=True)
            self.assertEqual(imported.returncode, 0, imported.stderr)

    def test_generator_rejects_changed_image_helper_and_unpassed_checkpoint(self):
        for fault in ('image', 'helper', 'checkpoint'):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                saved = artifact(root/'artifact')
                target = root/'collector.py'
                if fault == 'image':
                    (saved/COLLECT.IMAGE).write_bytes(b'changed image')
                elif fault == 'helper':
                    (saved/'runtime/usr/sbin/q1000k-pon-bench').write_text('changed helper')
                else:
                    record = json.loads((saved/'checkpoint.json').read_text())
                    record['status'] = 'failed'
                    (saved/'checkpoint.json').write_text(json.dumps(record))
                with self.assertRaises(ValueError):
                    COLLECT.build_single_file(target, saved)
                self.assertFalse(target.exists())

    def test_baseline_mode_needs_explicit_physical_confirmation(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            COLLECT.main(['--baseline-only', '--inputs', '/missing', '--output', '/missing'])
        plan = COLLECT.plan(True)
        self.assertEqual(plan['mode'], 'baseline-only')
        self.assertEqual(len(plan['cases']), 1)
        self.assertFalse(plan['live_reconnection_tested'])

    def test_different_collectors_share_device_lock(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(COLLECT.tempfile, 'gettempdir', return_value=directory):
            with COLLECT.device_lock():
                with self.assertRaises(BlockingIOError):
                    with COLLECT.device_lock():
                        self.fail('Concurrent collectors cannot own the device')


class PhysicalControlTests(unittest.TestCase):
    def test_prompts_follow_observed_phases_and_explicit_confirmations(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            control = COLLECT.LiveControl(Path(directory), COLLECT.Stop())
            light = rows(False)
            with patch.object(COLLECT, 'read_answer') as answer:
                control.advance(light[:14])
                self.assertEqual(control.state, 'lit')
                answer.assert_not_called()
                control.advance(light)
                self.assertEqual(control.state, 'confirm-dark')
                answer.return_value = None
                dark = light + rows(True, 15, irq=1)
                control.advance(dark)
                self.assertEqual(control.state, 'confirm-dark')
                answer.return_value = 'DISCONNECTED'
                control.advance(dark)
                self.assertEqual(control.state, 'dark')
                control.advance(dark)
                self.assertEqual(control.state, 'confirm-reconnected')
                answer.return_value = 'CONNECTED'
                relit = dark + rows(False, 30, irq=2)
                control.advance(relit)
                control.advance(relit)
            result = control.result()
            self.assertEqual(result['status'], 'complete')
            self.assertEqual([phase['first_sample'] for phase in result['observed_phases']], [1, 16, 31])
            self.assertEqual(result['observed_phases'][-1]['irq_change_since_previous_phase_end'], 1)
            self.assertNotIn('latency_ms', result['observed_phases'][-1])
            self.assertTrue((Path(directory)/'operator-events.json').exists())

    def test_wrong_or_missing_answer_never_confirms_a_physical_state(self):
        for answer in ('yes', None, 'STOP'):
            with self.subTest(answer=answer), tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
                control = COLLECT.LiveControl(Path(directory), COLLECT.Stop())
                control.advance(rows(False))
                with patch.object(COLLECT, 'read_answer', return_value=answer):
                    control.advance(rows(False)+rows(True, 15)+rows(False, 30))
                self.assertEqual(control.result()['status'], 'incomplete')
                self.assertFalse(any(event.get('action') == 'DISCONNECTED' for event in control.events))

    def test_phase_requires_both_los_sources_and_consecutive_samples(self):
        sample = rows(False, count=29)
        sample[14]['phy_los'] = True
        self.assertIsNone(COLLECT.consecutive_phase(sample, 0, False))
        sample.extend(rows(False, 29, count=1))
        self.assertEqual(COLLECT.consecutive_phase(sample, 0, False)['first_sample'], 16)

    def test_partial_last_log_line_is_not_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory)/'attempt.log'
            log.write_text(json.dumps(dict(rx_bench=True, **rows(False)[0]))+'\n{"rx_bench": true')
            self.assertEqual(len(COLLECT.rx_rows(log)), 1)

    def test_finished_window_neither_prompts_nor_accepts_late_confirmation(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            control = COLLECT.LiveControl(Path(directory), COLLECT.Stop())
            with patch.object(COLLECT, 'read_answer', return_value='DISCONNECTED') as answer:
                control.advance(rows(False), allow_prompts=False)
                self.assertEqual(control.state, 'lit')
                control.advance(rows(False))
                self.assertEqual(control.state, 'confirm-dark')
                control.advance(rows(False)+rows(True, 15), allow_prompts=False)
                self.assertEqual(control.state, 'confirm-dark')
                answer.assert_not_called()


class WorkflowTests(unittest.TestCase):
    def args(self, root, baseline=False):
        inputs = root/'private-inputs.tar'
        inputs.write_bytes(b'PRIVATE INPUT BYTES MUST NOT BE ARCHIVED')
        serial = root/'serial.log'
        serial.write_text('active logger fixture\n')
        return argparse.Namespace(output=root/'capture', inputs=inputs, serial_log=serial,
                                  serial_device=None, baseline_only=baseline, fiber_connected=baseline)

    def run_fixture(self, root, args, *, confirmations=None, summary=None, capture=None):
        saved = artifact(root/'artifact')
        original_module = COLLECT.module
        runner = Mock()
        def load(source, name):
            return runner if name == 'bench-run' else original_module(source, name)
        def capture_ok(args, source, saved, case, stop):
            path = args.output/case['name']
            path.mkdir()
            (path/'serial.log').write_text('kernel fixture\n')
            (path/'attempt.log').write_text('raw fixture\n')
            return None
        with contextlib.redirect_stdout(io.StringIO()), patch.object(COLLECT, 'module', side_effect=load), \
                patch.object(COLLECT, 'confirm', side_effect=confirmations or [True, True]), \
                patch.object(COLLECT, 'capture_case', side_effect=capture or capture_ok) as run, \
                patch.object(COLLECT, 'summarize_case', side_effect=summary or [dict(status='observed'), dict(status='observed')]):
            status = COLLECT.execute(args, ROOT, saved, dict(revision='a'*40))
        return status, run.call_count, json.loads((args.output/'collection.json').read_text())

    def test_failure_retains_raw_evidence_and_skips_next_stage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.args(root)
            status, calls, record = self.run_fixture(root, args, summary=[ValueError('cleanup failed')])
            self.assertEqual(status, 1)
            self.assertEqual(calls, 1)
            self.assertEqual(record['results'][0]['status'], 'failed')
            self.assertEqual(record['not_run'], ['live-reconnect'])
            with tarfile.open(root/'capture.tar.gz') as archive:
                names = archive.getnames()
                self.assertIn('capture/connected-baseline/attempt.log', names)
                self.assertFalse(any('input' in name for name in names))
                self.assertNotIn(b'PRIVATE INPUT BYTES', b''.join(archive.extractfile(name).read() for name in names))

    def test_input_closing_before_first_case_creates_partial_archive_without_running(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            status, calls, record = self.run_fixture(root, self.args(root), confirmations=[EOFError('stdin closed')])
            self.assertEqual((status, calls), (1, 0))
            self.assertEqual(record['not_run'], ['connected-baseline', 'live-reconnect'])
            self.assertTrue((root/'capture.tar.gz').is_file())

    def test_interruption_after_capture_stops_before_next_stage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def interrupted(args, source, saved, case, stop):
                stop.request('interrupted fixture')
                return None
            status, calls, record = self.run_fixture(root, self.args(root), capture=interrupted)
            self.assertEqual((status, calls), (1, 1))
            self.assertEqual(record['results'][0]['status'], 'interrupted')
            self.assertEqual(record['not_run'], ['live-reconnect'])
            self.assertTrue((root/'capture.tar.gz').is_file())

    def test_empty_serial_cannot_be_reported_as_valid(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'serial.log').write_bytes(b'')
            with patch.object(COLLECT, 'module') as load, self.assertRaisesRegex(ValueError, 'Serial evidence is empty'):
                COLLECT.summarize_case(ROOT, root)
            load.assert_not_called()

    def test_archive_allowlist_ignores_private_files_and_rejects_symlinked_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root/'capture'
            output.mkdir()
            private = root/'private-inputs.tar'
            private.write_bytes(b'PRIVATE')
            (output/'unexpected-private-copy.bin').write_bytes(b'PRIVATE')
            (output/'collection.json').write_text('{}')
            (output/'serial-source.log').write_text('serial setup evidence\n')
            record = dict(COLLECT.plan(True), serial_device='/dev/fixture')
            result = COLLECT.package_capture(output, record)
            with tarfile.open(result) as archive:
                self.assertEqual(archive.getnames(), ['capture/collection.json', 'capture/serial-source.log'])
            result.unlink()
            result = COLLECT.package_capture(output, COLLECT.plan(True))
            with tarfile.open(result) as archive:
                self.assertNotIn('capture/serial-source.log', archive.getnames())
            result.unlink()
            (output/'collection.json').unlink()
            (output/'collection.json').symlink_to(private)
            with self.assertRaisesRegex(ValueError, 'regular file'):
                COLLECT.package_capture(output, COLLECT.plan(True))

    def test_failed_serial_stops_at_confirmation_and_before_launch(self):
        serial = Mock()
        serial.check.side_effect = RuntimeError('serial disconnected')
        with contextlib.redirect_stdout(io.StringIO()), patch.object(COLLECT, 'read_answer') as answer:
            with self.assertRaisesRegex(RuntimeError, 'serial disconnected'):
                COLLECT.confirm('Ready?', 'CONNECTED', COLLECT.Stop(), [], serial)
            answer.assert_not_called()
        with patch.object(COLLECT.subprocess, 'Popen') as process:
            with self.assertRaisesRegex(RuntimeError, 'serial disconnected'):
                COLLECT.capture_case(argparse.Namespace(serial_capture=serial), ROOT, Path('/artifact'), {}, COLLECT.Stop())
            process.assert_not_called()


if __name__ == '__main__':
    unittest.main()
