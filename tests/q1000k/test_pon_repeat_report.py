#!/usr/bin/env python3
"""Bounded recovery evidence: no missing resets or invented cross-reset activity."""
import argparse
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from test_pon_bench_suite import PROBE, SUITE
from test_pon_bench_collect import COLLECT


class RepeatReportTests(unittest.TestCase):
    def serial(self, attempts=6, reason='budget'):
        lines=[]
        for n in range(1,attempts+1):
            for phase in ('before','after'):
                after=phase=='after'
                row=dict(sampled_ms=10000+(n-1)*6000+int(after),synced=0,controller_los=0,phy_los=0,
                         frames=0 if after else 3,lof=0,fec_total=0 if after else 9,
                         fec_corrected=0,fec_uncorrected=0,cw_start=0 if after else 12,cw_end=0,
                         sof_to_mac=0,eof_to_mac=0,psync_mismatch=0,sfc_hec_error=0,
                         pon_id_hec_error=0,ncpo=0xfedcba98,writes=(n-1+int(after))*31)
                lines.append(f'[123.45] q1000k: RX recovery phase={phase} attempt={n} '+
                             ' '.join(f'{k}={v}' for k,v in row.items())+'\n')
        lines.append(f'q1000k: RX recovery stopped reason={reason} attempts={attempts}\n')
        return ''.join(lines)

    def test_six_attempts_retain_boundaries_and_only_inter_reset_deltas(self):
        result=PROBE.recovery_series(self.serial(),'oem-reset-repeat',6)
        self.assertEqual(len(result['boundaries']),6)
        self.assertEqual(len(result['acquisition_intervals']),5)
        self.assertFalse(result['deltas_cross_reset'])
        for interval in result['acquisition_intervals']:
            self.assertEqual(interval['elapsed_ms'],5999)
            self.assertEqual(interval['counter_delta_mod32']['frames'],3)
            self.assertEqual(interval['counter_delta_mod32']['fec_total'],9)
            self.assertEqual(interval['counter_delta_mod32']['cw_start'],12)

    def test_early_sync_including_before_first_attempt(self):
        for n in (0,1,5):
            result=PROBE.recovery_series(self.serial(n,'sync'),'oem-reset-repeat',n)
            self.assertEqual(result['attempts'],n)
            self.assertEqual(result['stop_reason'],'sync')
        # The post-reset sample latches sync before printing its after record.
        lines=self.serial(1,'sync').splitlines(True)
        lines[1]=lines[1].replace('synced=0','synced=1')
        result=PROBE.recovery_series(lines[0]+lines[2]+lines[1],'oem-reset-repeat',1)
        self.assertEqual(result['boundaries'][0]['after']['synced'],1)

    def test_truncated_duplicate_out_of_order_or_failed_records_rejected(self):
        original=self.serial()
        lines=original.splitlines(True)
        cases=[original.replace(lines[1],''), original+lines[0], ''.join(lines[1:2]+lines[:1]+lines[2:]),
               original.replace('phase=after','phase=unknown',1),
               original.replace('stopped reason=budget attempts=6','stopped reason=budget attempts=5'),
               original+'q1000k: RX recovery failure attempt=6 error=-5\n',
               original.replace('writes=31','writes=0',1),
               original.replace('ncpo=4275878552','ncpo=4294967296',1)]
        for serial in cases:
            with self.subTest(serial=serial), self.assertRaises(ValueError):
                PROBE.recovery_series(serial,'oem-reset-repeat',6)

    def test_timing_and_latched_stop_conditions(self):
        original=self.serial()
        for serial in (original.replace('sampled_ms=16000','sampled_ms=14000'),
                       original.replace('phase=after attempt=1 sampled_ms=10001 synced=0',
                                        'phase=after attempt=1 sampled_ms=10001 synced=1'),
                       original.replace('controller_los=0','controller_los=1',1),
                       original.splitlines(True)[-1]+original[:-len(original.splitlines(True)[-1])]):
            with self.assertRaises(ValueError): PROBE.recovery_series(serial,'oem-reset-repeat',6)
        self.assertIsNone(PROBE.recovery_series('','oem-full-reset',1))
        with self.assertRaises(ValueError): PROBE.recovery_series(original,'oem-full-reset',1)

    def test_plan_preserves_controls_and_promotes_short_repeat_window(self):
        plan=SUITE.plan(30)
        self.assertEqual(len(plan['cases']),56)
        self.assertEqual(len(COLLECT.plan()['cases']),58)
        cases={c['name']:c for c in plan['cases']}
        self.assertEqual(cases['baseline']['samples'],30)
        self.assertEqual(cases['oem-full-reset']['samples'],30)
        self.assertEqual(cases['oem-reset-repeat']['samples'],90)
        self.assertEqual(SUITE.plan(180,'oem-reset-repeat')['cases'][0]['samples'],180)
        for version in range(1,5):
            with self.assertRaises(ValueError):
                SUITE.check_artifact_compatibility([cases['oem-reset-repeat']],version)
        SUITE.check_artifact_compatibility([cases['oem-full-reset']],4)
        SUITE.check_artifact_compatibility(plan['cases'],5)

    def test_direct_runner_rejects_missing_short_or_old_artifact_before_access(self):
        for count in (None,30,90):
            args=argparse.Namespace(probe='oem-reset-repeat',samples=count,artifact=Path('/artifact'))
            with patch.object(SUITE.RUN,'diagnostics_version',return_value=5):
                if count==90: SUITE.RUN.validate_experiment_artifact(args)
                else:
                    with self.assertRaisesRegex(ValueError,'90 samples'):
                        SUITE.RUN.validate_experiment_artifact(args)

    def test_complete_capture_groups_checker_by_attempt(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)
            record=dict(diagnostics_version=5,probe='oem-reset-repeat',reacquire_once=True,samples=90)
            (path/'checkpoint.json').write_text(json.dumps(record))
            rows=[]
            for i in range(90):
                n=min(6,max(0,(i-10)//6+1))
                rows.append(dict(rx_bench=True,reacquire_attempts=n,sampled_ms=i*1000))
                rows.append(dict.fromkeys(PROBE.FIELDS,0) | dict(diagnostics_version=5,probe=38,
                    attempts=n,writes=n*31,sampled_ms=i*1000+1,checker_errors=i))
            (path/'attempt.log').write_text(''.join(json.dumps(r)+'\n' for r in rows))
            (path/'serial.log').write_text(self.serial()+'q1000k: RX probe fields restored\n')
            report=PROBE.summarize(path)
            self.assertEqual(report['attempts'],6)
            self.assertIsNone(report['checker_error_phases']['after'])
            self.assertEqual(len(report['attempt_phases']),7)
            self.assertEqual(report['attempt_phases']['1']['delta_mod32'],5)
            self.assertTrue(report['restoration_confirmed'])


if __name__=='__main__': unittest.main()
