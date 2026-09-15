#!/usr/bin/env python3
"""Physical controls keep light/dark phases separate and retain safety checks."""
import json
from pathlib import Path
import tempfile
import unittest
import test_pon_bench_report as base
import test_pon_bench_matrix as matrix
import test_pon_bench_suite as suite

CONTROL = matrix.MATRIX.module('bench-control-report')


class ControlTests(unittest.TestCase):
    def fixture(self, path):
        matrix.DiagnosticTests().fixture(path, version=5)
        diagnostic_rows = [json.loads(line) for line in (path/'attempt.log').read_text().splitlines()]
        receiver = [r for r in diagnostic_rows if 'rx_bench' in r]
        controller_words = diagnostic_rows[0]
        helper = base.ReportTests()
        helper.capture = path
        record, controller, omci, samples = helper.fixture(fiber='connected')
        record.update(status='failed', error='SSH failed (1); see attempt.log',
                      diagnostics_version=1, probe='checker', reacquire_once=True)
        for n, row in enumerate(samples):
            row.update(receiver[n])
            row.update(reacquire_enabled=True, reacquire_attempts=int(n>=20),
                       controller_los=23<=n<27, phy_los=23<=n<27,
                       rx_power_nw=100 if 23<=n<27 else 14100)
        helper.save(record, controller, omci, samples)
        lines = [json.dumps(controller_words)]
        for line in (path/'attempt.log').read_text().splitlines():
            lines.append(line)
            if not line.startswith('{'):
                continue
            row = json.loads(line)
            if 'rx_bench' not in row:
                continue
            attempts = row['reacquire_attempts']
            diag = dict.fromkeys(suite.PROBE.FIELDS, 0)
            diag.update(diagnostics_version=1, probe=10, attempts=attempts,
                        writes=2*attempts, sampled_ms=row['sampled_ms']+1,
                        checker_control=5+65536*attempts,
                        checker_event=65793 if attempts else 256,
                        checker_errors=65529 if attempts else 0)
            lines.extend([json.dumps(diag), json.dumps(controller_words)])
        lines.append('Q1000K bench: Downstream LOS/sync/frame stability was not established.')
        (path/'attempt.log').write_text('\n'.join(lines)+'\n')
        (path/'serial.log').write_text('q1000k: RX probe fields restored\n')

    def test_disconnect_and_reconnect_preserve_latched_result_without_claiming_service(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            self.fixture(path)
            report, _, _ = CONTROL.summarize(path)
            phases = report['phases']
            self.assertEqual([p['samples'] for p in phases], [20, 3, 4, 3])
            self.assertEqual([p['controller_los'] for p in phases], [False, False, True, False])
            self.assertEqual([p['optical']['rx_power_nw']['last'] for p in phases], [14100, 14100, 100, 14100])
            self.assertEqual([p['diagnostics']['checker_errors'] for p in phases], [[0], [65529], [65529], [65529]])
            self.assertFalse(report['optical_service_verified'])
            self.assertEqual(report['cleanup'], 'passed')

    def test_physical_control_does_not_relax_capture_or_tx_guards(self):
        for fault in ('cleanup', 'tx', 'short', 'restore'):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                path = Path(directory)
                self.fixture(path)
                if fault == 'cleanup':
                    checkpoint = path/'checkpoint.json'
                    record = json.loads(checkpoint.read_text())
                    record['postflight'] = 'failed'
                    checkpoint.write_text(json.dumps(record))
                elif fault == 'restore':
                    (path/'serial.log').write_text('')
                else:
                    attempt = path/'attempt.log'
                    text = attempt.read_text()
                    if fault == 'tx':
                        text = text.replace('"tx_enabled": false', '"tx_enabled": true', 1)
                    else:
                        text = '\n'.join(line for line in text.splitlines() if '"sampled_ms": 30001' not in line)
                    attempt.write_text(text)
                with self.assertRaises(ValueError):
                    CONTROL.summarize(path)


if __name__ == '__main__':
    unittest.main()
