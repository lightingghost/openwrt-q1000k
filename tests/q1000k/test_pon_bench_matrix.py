#!/usr/bin/env python3
"""A matrix can continue after missing sync, never after incomplete safe cleanup."""
import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('matrix', ROOT / 'scripts/q1000k/bench-matrix.py')
MATRIX = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MATRIX)


def observation(stable=False):
    return dict(cleanup='passed', fiber='connected', receive=dict(
        downstream_stable=stable, reacquire_requested=False, reacquire_attempts=0,
        controller_los=[False], phy_los=[False]))


class MatrixTests(unittest.TestCase):
    def test_safe_baseline_selects_observation_or_single_recovery(self):
        self.assertEqual(MATRIX.followup(observation()), ('single-reacquire', True))
        self.assertEqual(MATRIX.followup(observation(True)), ('extended-observe', False))
        for problem in ('cleanup', 'light', 'already-recovered'):
            report = observation()
            if problem == 'cleanup':
                report['cleanup'] = 'failed'
            elif problem == 'light':
                report['receive']['phy_los'] = [False, True]
            else:
                report['receive']['reacquire_attempts'] = 1
            with self.assertRaises(ValueError):
                MATRIX.followup(report)

    def test_matrix_never_retries_a_stage_or_advances_after_safety_failure(self):
        for case in ('recovery', 'stable', 'bad-baseline', 'bad-followup'):
            with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
                args = argparse.Namespace(output=Path(directory) / 'capture', artifact=Path('/artifact'),
                                          extended_samples=90)
                first = ValueError('unsafe') if case == 'bad-baseline' else observation(case == 'stable')
                second = ValueError('unsafe') if case == 'bad-followup' else observation(case == 'stable')
                with patch.object(MATRIX, 'stage', side_effect=[first, second]) as stage:
                    result = MATRIX.execute(args)
                record = json.loads((args.output / 'matrix.json').read_text())
                self.assertEqual(stage.call_count, 1 if case == 'bad-baseline' else 2)
                self.assertEqual(record['status'], 'stopped' if case.startswith('bad-') else 'completed')
                self.assertEqual(result, 0 if case == 'stable' else 1)
                self.assertFalse(record['optical_service_verified'])
                if stage.call_count == 2:
                    self.assertEqual(stage.call_args.args[2:], (90, case != 'stable'))

    def test_stage_requires_both_full_observation_and_receiver_evidence(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            args = argparse.Namespace(output=Path(directory), artifact=Path('/artifact'),
                                      inputs=Path('/inputs'), serial_log=Path('/serial'))
            with patch.object(MATRIX.RUN, 'main', return_value=1) as run:
                with patch.object(MATRIX.REPORT, 'summarize', side_effect=ValueError('missing cleanup')):
                    with patch.object(MATRIX.RECEIVER, 'summarize') as diagnostic:
                        with self.assertRaises(ValueError):
                            MATRIX.stage(args, 'baseline', 30, False)
                        diagnostic.assert_not_called()
                self.assertNotIn('--reacquire-once', run.call_args.args[0])


class DiagnosticTests(unittest.TestCase):
    def fixture(self, path, count=30, version=2):
        record=dict(schema_version=1, action='receive', host='192.168.255.1', fiber='connected',
                    status='failed', postflight='passed', input_cleanup='passed', revision='a'*40,
                    samples=count, started=0, finished=200)
        names = MATRIX.RECEIVER.PHY_WORDS + (MATRIX.RECEIVER.EXTENDED_PHY_WORDS if version == 2 else ())
        controller = dict(receiver_status=True, **{key: 0 for key in MATRIX.RECEIVER.CONTROLLER_WORDS})
        rx=dict(rx_bench=True, tx_inhibited=True, tx_enabled=False, registration_enabled=False,
                mac_irq_mask=0, sync_status=0, controller_los=False, phy_los=False, synced=False,
                receiver_version=version, receiver={key: 0 for key in names},
                **{key: 0 for key in MATRIX.RECEIVER.COUNTERS})
        rows=[controller]
        for n in range(count):
            rows += [controller, dict(rx, sampled_ms=(n+1)*1000, poll_calls=n)]
        (path/'checkpoint.json').write_text(json.dumps(record))
        (path/'attempt.log').write_text('\n'.join(map(json.dumps, rows)))
        return rows

    def test_versioned_diagnostics_and_extended_windows(self):
        for count, version in ((30, 1), (90, 2), (180, 2)):
            with tempfile.TemporaryDirectory() as directory:
                path=Path(directory)
                self.fixture(path, count, version)
                report=MATRIX.RECEIVER.summarize(path)
                self.assertEqual(report['rx_samples'], count)
                self.assertEqual(report['bench_result'], 'failed')
                self.assertEqual(len(report['phy_words']), 24 if version == 2 else 13)

    def test_partial_extended_diagnostic_is_rejected(self):
        for problem in ('missing-word', 'bad-word', 'mixed-version', 'missing-sample'):
            with tempfile.TemporaryDirectory() as directory:
                path=Path(directory)
                rows=self.fixture(path)
                if problem == 'missing-word':
                    del rows[-1]['receiver']['pll_pcw2']
                elif problem == 'bad-word':
                    rows[-1]['receiver']['rx_lock_force']=0xffffffff
                elif problem == 'mixed-version':
                    rows[-1]['receiver_version']=1
                else:
                    rows.pop()
                (path/'attempt.log').write_text('\n'.join(map(json.dumps, rows)))
                with self.assertRaises(ValueError):
                    MATRIX.RECEIVER.summarize(path)


if __name__ == '__main__':
    unittest.main()
