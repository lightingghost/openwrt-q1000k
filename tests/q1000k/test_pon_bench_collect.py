#!/usr/bin/env python3
"""Portable collection remains offline, stops on faults and preserves evidence."""
import argparse
import contextlib
import gzip
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


def artifact(path, version=3):
    path.mkdir()
    image = b'fixture RAM image'
    helper = f'RX_DIAGNOSTICS_VERSION={version}\n'.encode()
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
            self.assertEqual(result['mode'], 'experiments')
            self.assertEqual(result['artifact']['revision'], 'a'*40)
            self.assertEqual(result['artifact']['diagnostics_version'], 3)
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

    def test_default_plan_covers_every_fixed_experiment_and_one_repeat(self):
        suite = COLLECT.module(ROOT, 'bench-suite')
        plan = COLLECT.plan()
        probes = [case['probe'] for case in plan['cases'] if case.get('probe') == case['name']]
        self.assertCountEqual(probes, suite.RUN.PROBES)
        self.assertEqual(plan['cases'][0]['name'], 'connected-baseline')
        self.assertEqual([case for case in plan['cases'] if case['fiber'] == 'connected'][-1]['name'], 'baseline-repeat')
        self.assertEqual(plan['cases'][-1]['name'], 'live-reconnect')
        self.assertFalse(plan['automatic_retries'])
        self.assertTrue(all(case['samples'] == 90 for case in plan['cases']
                            if case.get('reacquire') and case['fiber'] == 'connected'))

    def test_explicit_case_is_one_bounded_rerun_and_unknown_case_rejected(self):
        selected = COLLECT.plan(selected_case='checker')
        self.assertEqual(len(selected['cases']), 1)
        self.assertEqual(selected['cases'][0]['probe'], 'checker')
        self.assertTrue(selected['cases'][0]['reacquire'])
        self.assertNotIn('live-reconnect', [case['name'] for case in COLLECT.plan(skip_live_control=True)['cases']])
        with self.assertRaisesRegex(ValueError, 'Unknown or excluded case'):
            COLLECT.plan(selected_case='arbitrary-register-write')

    def test_old_portable_artifact_rejects_new_modes_and_retains_legacy_baseline(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            root = Path(directory)
            saved = artifact(root/'artifact', version=2)
            script = root/'collector.py'
            COLLECT.build_single_file(script, saved)
            default = subprocess.run([sys.executable, str(script), '--dry-run'], capture_output=True, text=True)
            self.assertEqual(default.returncode, 1)
            self.assertIn('schema 3 is required', default.stderr)
            legacy = subprocess.run([sys.executable, str(script), '--dry-run', '--baseline-only'],
                                    capture_output=True, text=True)
            self.assertEqual(legacy.returncode, 0, legacy.stderr)
            self.assertEqual(json.loads(legacy.stdout)['artifact']['diagnostics_version'], 2)


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

    def test_fresh_dark_checker_requires_post_arm_dark_samples_before_reconnection(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            control = COLLECT.FreshCheckerControl(Path(directory), COLLECT.Stop())
            light = [dict(row, reacquire_attempts=0) for row in rows(False)]
            pending = light + [dict(row, reacquire_attempts=0) for row in rows(True, 15)]
            armed = pending + [dict(row, reacquire_attempts=1) for row in rows(True, 30)]
            reconnected = armed + [dict(row, reacquire_attempts=1) for row in rows(False, 45)]
            control.advance(light)
            with patch.object(COLLECT, 'read_answer', return_value='DISCONNECTED'):
                control.advance(pending)
            control.advance(pending)
            self.assertEqual(control.state, 'dark')
            control.advance(armed[:-1])
            self.assertEqual(control.state, 'dark')
            control.advance(armed)
            self.assertEqual(control.state, 'confirm-reconnected')
            with patch.object(COLLECT, 'read_answer', return_value='CONNECTED'):
                control.advance(reconnected)
            control.advance(reconnected)
            self.assertEqual(control.result()['status'], 'complete')
            self.assertEqual(control.result()['probe'], 'checker-dark')
            self.assertEqual([phase['first_sample'] for phase in control.phases], [1, 31, 46])


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
                patch.object(COLLECT, 'confirm', side_effect=confirmations or [True]), \
                patch.object(COLLECT, 'capture_case', side_effect=capture or capture_ok) as run, \
                patch.object(COLLECT, 'summarize_case', side_effect=summary or [dict(status='observed'), dict(status='observed')]):
            status = COLLECT.execute(args, ROOT, saved, dict(revision='a'*40, diagnostics_version=3))
        return status, run.call_count, json.loads((args.output/'collection.json').read_text())

    def test_failure_retains_raw_evidence_and_skips_next_stage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.args(root)
            status, calls, record = self.run_fixture(root, args, summary=[ValueError('cleanup failed')])
            self.assertEqual(status, 1)
            self.assertEqual(calls, 1)
            self.assertEqual(record['results'][0]['status'], 'failed')
            self.assertEqual(record['not_run'], [case['name'] for case in COLLECT.plan()['cases'][1:]])
            with tarfile.open(root/'capture.tar.gz') as archive:
                names = archive.getnames()
                self.assertIn('capture/connected-baseline/attempt.log', names)
                self.assertFalse(any('input' in name for name in names))
                self.assertNotIn(b'PRIVATE INPUT BYTES', b''.join(archive.extractfile(name).read() for name in names))

    def test_old_artifact_is_rejected_before_inputs_output_or_capture_access(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.args(root)
            saved = artifact(root/'artifact', version=2)
            args.inputs = root/'missing-private-inputs.tar'
            with patch.object(COLLECT, 'capture_case') as capture:
                with self.assertRaisesRegex(ValueError, 'schema 3 is required'):
                    COLLECT.execute(args, ROOT, saved, dict(revision='a'*40, diagnostics_version=2))
            capture.assert_not_called()
            self.assertFalse(args.output.exists())

    def test_all_connected_experiments_share_one_archive_and_continue_after_no_sync(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.args(root)
            args.skip_live_control = True
            expected = COLLECT.plan(skip_live_control=True)['cases']
            status, calls, record = self.run_fixture(root, args,
                summary=[dict(status='observed', downstream_stable=False) for _ in expected])
            self.assertEqual((status, calls), (0, len(expected)))
            self.assertEqual(record['not_run'], [])
            self.assertEqual([case['name'] for case in record['results']], [case['name'] for case in expected])
            with tarfile.open(root/'capture.tar.gz') as archive:
                for case in expected:
                    self.assertIn('capture/'+case['name']+'/attempt.log', archive.getnames())

    def test_selected_experiment_passes_only_the_fixed_probe_and_one_recovery(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            root = Path(directory)
            args = argparse.Namespace(output=root, inputs=root/'inputs.tar',
                                      serial_log=root/'serial.log', serial_capture=None)
            case = COLLECT.plan(selected_case='checker')['cases'][0]
            process = Mock()
            process.poll.return_value = 0
            with patch.object(COLLECT.subprocess, 'Popen', return_value=process) as launch:
                COLLECT.capture_case(args, ROOT, root/'artifact', case, COLLECT.Stop())
            command = launch.call_args.args[0]
            self.assertEqual(command[command.index('--probe')+1], 'checker')
            self.assertEqual(command.count('--reacquire-once'), 1)
            self.assertEqual(command[command.index('--samples')+1], '90')
            self.assertTrue(launch.call_args.kwargs['start_new_session'])

    def test_integrated_controller_selection_is_explicit_in_command_and_checkpoint(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            root = Path(directory)
            args = argparse.Namespace(output=root, inputs=root/'inputs.tar',
                                      serial_log=root/'serial.log', serial_capture=None)
            case = COLLECT.plan(selected_case='oem-md32-acquire-600-flat')['cases'][0]
            process = Mock()
            process.poll.return_value = 0
            with patch.object(COLLECT.subprocess, 'Popen', return_value=process) as launch:
                COLLECT.capture_case(args, ROOT, root/'artifact', case, COLLECT.Stop())
            command = launch.call_args.args[0]
            self.assertEqual(command[command.index('--probe')+1], 'oem-rx-acquire')
            self.assertEqual(command.count('--reacquire-once'), 1)
            self.assertIn('--oem-md32', command)
            self.assertEqual(command[command.index('--rx-output')+1], '600-flat')
            suite = COLLECT.module(ROOT, 'bench-suite')
            (root/'checkpoint.json').write_text(json.dumps(dict(oem_md32=True, rx_output='600-flat')))
            suite.check_controller_selection(root, case)
            (root/'checkpoint.json').write_text(json.dumps(dict(oem_md32=False, rx_output='600-flat')))
            with self.assertRaisesRegex(ValueError, 'Controller experiment selection'):
                suite.check_controller_selection(root, case)

    def test_summary_accepts_only_the_planned_experiment(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory)
            (capture/'serial.log').write_text('kernel fixture\n')
            (capture/'checkpoint.json').write_text('{}')
            control = COLLECT.module(ROOT, 'bench-control-report')
            observed = dict(cleanup='passed', fiber='connected',
                receive=dict(controller_los=[False], phy_los=[False], reacquire_requested=True,
                             downstream_stable=False),
                probe_diagnostics=dict(probe='checker', attempts=1))
            with patch.object(COLLECT, 'module', return_value=control), \
                 patch.object(control, 'summarize', return_value=({}, observed, {})), \
                 patch.object(control.SUITE.HYPOTHESES, 'evaluate', return_value={}):
                result = COLLECT.summarize_case(ROOT, capture, case=dict(probe='checker', reacquire=True))
                self.assertEqual((result['status'], result['attempts']), ('observed', 1))
                with self.assertRaisesRegex(ValueError, 'selection'):
                    COLLECT.summarize_case(ROOT, capture, case=dict(probe='bit-order', reacquire=True))

    def test_fresh_dark_summary_rechecks_post_arm_phases_in_raw_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory)
            (capture/'serial.log').write_text('kernel fixture\n')
            (capture/'checkpoint.json').write_text('{}')
            control = COLLECT.module(ROOT, 'bench-control-report')
            observed = dict(cleanup='passed', fiber='connected',
                receive=dict(controller_los=[False, True], phy_los=[False, True], reacquire_requested=True,
                             downstream_stable=False),
                probe_diagnostics=dict(probe='checker-dark', attempts=1))
            live = dict(status='complete', observed_phases=[dict(name=name) for name in ('lit', 'dark', 'reconnected')])
            case = dict(probe='checker-dark', reacquire=True)
            with patch.object(COLLECT, 'module', return_value=control), \
                 patch.object(control, 'summarize', return_value=({}, observed, {})), \
                 patch.object(control.SUITE.HYPOTHESES, 'evaluate', return_value={}):
                for dark_attempts in (1, 0):
                    samples = [dict(row, rx_bench=True, reacquire_attempts=attempts)
                               for los, start, attempts in ((False, 0, 0), (True, 15, dark_attempts), (False, 30, 1))
                               for row in rows(los, start)]
                    (capture/'attempt.log').write_text(''.join(json.dumps(row)+'\n' for row in samples))
                    if dark_attempts:
                        self.assertEqual(COLLECT.summarize_case(ROOT, capture, live, case)['status'], 'observed')
                    else:
                        with self.assertRaisesRegex(ValueError, 'Fresh checker arm|Complete capture'):
                            COLLECT.summarize_case(ROOT, capture, live, case)
                # A fabricated completed workflow must not mask a light-time arm.
                samples = [dict(row, rx_bench=True, reacquire_attempts=attempts)
                           for los, start, attempts in ((False, 0, 0), (False, 15, 1),
                                                        (True, 30, 1), (False, 45, 1))
                           for row in rows(los, start)]
                (capture/'attempt.log').write_text(''.join(json.dumps(row)+'\n' for row in samples))
                with self.assertRaisesRegex(ValueError, 'Fresh checker arm'):
                    COLLECT.summarize_case(ROOT, capture, live, case)

    def test_input_closing_before_first_case_creates_partial_archive_without_running(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            status, calls, record = self.run_fixture(root, self.args(root), confirmations=[EOFError('stdin closed')])
            self.assertEqual((status, calls), (1, 0))
            self.assertEqual(record['not_run'], [case['name'] for case in COLLECT.plan()['cases']])
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
            self.assertEqual(record['not_run'], [case['name'] for case in COLLECT.plan()['cases'][1:]])
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

    def test_archive_rejects_case_directory_symlink_to_private_material(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root/'capture'
            output.mkdir()
            private = root/'private'
            private.mkdir()
            (private/'attempt.log').write_bytes(b'PRIVATE PAYLOAD')
            (output/'connected-baseline').symlink_to(private, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, 'regular file'):
                COLLECT.package_capture(output, COLLECT.plan(True))
            with gzip.open(root/'capture.tar.gz', 'rb') as archive:
                self.assertEqual(archive.read(), b'')


if __name__ == '__main__':
    unittest.main()
