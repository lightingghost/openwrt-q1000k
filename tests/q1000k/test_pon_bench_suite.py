#!/usr/bin/env python3
"""All cases run after expected no-sync, never after a guard or cleanup error."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from test_pon_bench_matrix import MATRIX

SUITE=MATRIX.module('bench-suite')
PROBE=MATRIX.module('bench-probe-report')


def observation(probe=None,reacquire=False):
    return dict(cleanup='passed',fiber='connected',receive=dict(
        downstream_stable=False,reacquire_requested=reacquire,
        controller_los=[False],phy_los=[False]),
        probe_diagnostics=dict(probe=probe,attempts=int(reacquire)))


class SuiteTests(unittest.TestCase):
    def test_every_hypothesis_has_executable_and_external_coverage(self):
        names={c[0] for c in SUITE.CASES}
        self.assertEqual(len(SUITE.COVERAGE),9)
        self.assertEqual(len(SUITE.CASES),len(SUITE.CONNECTED_PROBES)+len(SUITE.CONTROLLER_CASES)+3)
        self.assertEqual(tuple(p for n,p,_ in SUITE.CASES if p == n),SUITE.CONNECTED_PROBES)
        for coverage in SUITE.COVERAGE.values():
            self.assertLessEqual(set(coverage['cases']),names)
            self.assertTrue(coverage['external'])

    def test_all_cases_continue_for_expected_no_sync_and_stop_on_fault(self):
        total=len(SUITE.CASES)
        for fail in (None,0,5,total-1):
            with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
                args=argparse.Namespace(output=Path(directory)/'suite',artifact=Path('/artifact'),samples=90)
                results=[ValueError('unsafe') if i==fail else observation(p,r)
                         for i,(_,p,r) in enumerate(SUITE.CASES)]
                with patch.object(SUITE,'stage',side_effect=results) as stage, \
                     patch.object(SUITE.RUN,'diagnostics_version',return_value=3):
                    status=SUITE.execute(args)
                self.assertEqual(stage.call_count,total if fail is None else fail+1)
                self.assertEqual(status,0 if fail is None else 1)
                record=json.loads((args.output/'suite.json').read_text())
                self.assertEqual(len(record['results']),total if fail is None else fail)
                self.assertFalse(record['optical_tx'])

    def test_explicit_case_does_not_repeat_other_experiments(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            args=argparse.Namespace(output=Path(directory)/'suite',artifact=Path('/artifact'),
                                    samples=90,case='checker')
            with patch.object(SUITE,'stage',return_value=observation('checker',True)) as stage, \
                 patch.object(SUITE.RUN,'diagnostics_version',return_value=3):
                self.assertEqual(SUITE.execute(args),0)
            self.assertEqual(stage.call_count,1)
            self.assertEqual(stage.call_args.args[1:],('checker','checker',True))
            record=json.loads((args.output/'suite.json').read_text())
            self.assertEqual(record['not_run'],[])
            self.assertFalse(record['automatic_retries'])

    def test_old_artifact_rejects_new_modes_before_capture_and_preserves_old_cases(self):
        for version in (1,2):
            SUITE.check_artifact_compatibility(SUITE.plan(90,'checker')['cases'],version)
            for name in ('cdr-auto-release','oem-md32','rx-output-600-flat','oem-md32-acquire-600-flat'):
                with self.subTest(version=version,case=name), self.assertRaisesRegex(ValueError,'schema 3 is required'):
                    SUITE.check_artifact_compatibility(SUITE.plan(90,name)['cases'],version)
        with tempfile.TemporaryDirectory() as directory:
            args=argparse.Namespace(output=Path(directory)/'suite',artifact=Path('/artifact'),samples=90)
            with patch.object(SUITE.RUN,'diagnostics_version',return_value=2), \
                 patch.object(SUITE,'stage') as stage, self.assertRaisesRegex(ValueError,'schema 3 is required'):
                SUITE.execute(args)
            stage.assert_not_called()
            self.assertFalse(args.output.exists())

    def test_mismatch_missing_attempt_and_lost_light_reject_case(self):
        for kind in ('attempt','light','cleanup','probe'):
            record=observation('checker',True)
            if kind=='attempt': record['probe_diagnostics']['attempts']=0
            elif kind=='light': record['receive']['phy_los']=[True,False]
            elif kind=='cleanup': record['cleanup']='failed'
            else: record['probe_diagnostics']['probe']='bit-order'
            with self.assertRaises(ValueError): SUITE.check_case(record,'checker',True)
        record=observation('checker',True)
        record['receive']['downstream_stable']=True
        record['probe_diagnostics']['attempts']=0
        self.assertEqual(SUITE.check_case(record,'checker',True),'not-triggered-already-synchronized')


class ProbeReportTests(unittest.TestCase):
    def fixture(self,path,version=2):
        (path/'checkpoint.json').write_text(json.dumps(dict(diagnostics_version=version,probe='checker',
            reacquire_once=True,restore_gain=False,restore_pll=False,samples=30)))
        rows=[]
        for i in range(30):
            attempts=int(i>=15)
            rows.append(dict(rx_bench=True,reacquire_attempts=attempts,sampled_ms=i*1000))
            rows.append(dict.fromkeys(PROBE.FIELDS_BY_VERSION[version],0) | dict(diagnostics_version=version,probe=10,
                attempts=attempts,writes=attempts*2,sampled_ms=i*1000+1,checker_control=5 | attempts*65536,
                checker_errors=i,rx_meter_lock_target=0xa4ff_a436,rx_meter_result=0xa49a_0303))
        (path/'serial.log').write_text('q1000k: RX probe fields restored\n')
        self.write(path,rows)
        return rows

    def write(self,path,rows):
        (path/'attempt.log').write_text(''.join(json.dumps(r)+'\n' for r in rows))

    def test_collects_raw_evidence_and_restoration(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory); self.fixture(path)
            result=PROBE.summarize(path)
            self.assertEqual(result['checked_writes'],2)
            self.assertIsNone(result['checker_error_delta_mod32'])
            self.assertEqual(result['checker_error_phases']['before']['delta_mod32'],14)
            self.assertEqual(result['checker_error_phases']['after']['delta_mod32'],14)
            self.assertTrue(result['restoration_confirmed'])
            self.assertEqual(result['rx_meter_upper16_within_configured_window'],[True])
            self.assertEqual(result['diagnostics_version'],2)

    def test_legacy_diagnostics_remain_readable_without_inventing_new_values(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory); self.fixture(path,version=1)
            result=PROBE.summarize(path)
            self.assertEqual(result['diagnostics_version'],1)
            self.assertNotIn('tdc_ncpo',result['raw_words'])
            self.assertIsNone(result['passive_clock_words'])

    def test_passive_words_keep_full_width_and_are_not_lock_assertions(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory); rows=self.fixture(path)
            for row in rows:
                if 'diagnostics_version' in row:
                    row['tdc_ncpo']=0xffffffff if row['attempts'] else 7
                    row['fifo_clock_status']=0xf1234567
            self.write(path,rows)
            result=PROBE.summarize(path)
            self.assertEqual(result['passive_clock_words']['tdc_ncpo']['maximum'],0xffffffff)
            self.assertEqual(result['passive_clock_words']['tdc_ncpo']['distinct_values'],2)
            self.assertEqual(result['passive_clock_words']['fifo_clock_status']['distinct_values'],1)

    def test_new_field_and_version_mismatches_are_rejected(self):
        for fault in ('missing','schema','overflow','bool'):
            with self.subTest(fault=fault),tempfile.TemporaryDirectory() as directory:
                path=Path(directory); rows=self.fixture(path)
                if fault=='missing': del rows[1]['tdc_ncpo']
                elif fault=='schema': rows[1]['diagnostics_version']=1
                elif fault=='overflow': rows[1]['fifo_clock_status']=0x100000000
                else: rows[1]['tdc_ncpo']=False
                self.write(path,rows)
                with self.assertRaises(ValueError): PROBE.summarize(path)

    def test_counter_restart_is_not_counted_as_activity(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory); rows=self.fixture(path)
            for row in rows:
                if 'checker_errors' in row:
                    row['checker_errors']=0 if row['attempts'] else 100
            self.write(path,rows)
            result=PROBE.summarize(path)
            self.assertIsNone(result['checker_error_delta_mod32'])
            self.assertEqual(result['checker_error_phases']['before']['delta_mod32'],0)
            self.assertEqual(result['checker_error_phases']['after']['delta_mod32'],0)

    def test_incomplete_stale_unsafe_and_missing_restore_rejected(self):
        for kind in ('short','missing','bool','stale','tx','loopback','mode','writes','restore','order'):
            with tempfile.TemporaryDirectory() as directory:
                path=Path(directory); rows=self.fixture(path)
                if kind=='short': rows.pop()
                elif kind=='missing': del rows[1]['checker_errors']
                elif kind=='bool': rows[1]['checker_errors']=False
                elif kind=='stale': rows[3]['sampled_ms']=0
                elif kind=='tx': rows[1]['checker_control']|=256
                elif kind=='loopback': rows[1]['data_route_control']=256
                elif kind=='mode': rows[1]['probe']=3
                elif kind=='writes': rows[-1]['writes']=0
                elif kind=='restore': (path/'serial.log').write_text('')
                elif kind=='order': rows[0],rows[1]=rows[1],rows[0]
                self.write(path,rows)
                with self.subTest(kind=kind), self.assertRaises(ValueError): PROBE.summarize(path)

if __name__=='__main__': unittest.main()
