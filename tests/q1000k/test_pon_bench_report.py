#!/usr/bin/env python3
"""Reject incomplete or unsafe saved bench observations without device access."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('bench_report', ROOT / 'scripts/q1000k/bench-report.py')
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


class ReportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.capture = Path(self.temp.name)

    def fixture(self, action='receive', fiber='disconnected', count=None):
        count = count or (30 if action == 'receive' else 5)
        record = dict(schema_version=1, action=action, fiber=fiber, host='192.168.255.1',
                      status='passed', postflight='passed', input_cleanup='passed',
                      revision='a' * 40, serial_start=0, started=1, finished=50)
        if action == 'receive':
            record['samples'] = count
        controller = dict(mode='xgspon', gpon_detected=True, xgspon_detected=True,
                          md32_enabled=True, tx_disabled=True, tx_inhibited=True,
                          calibration_supplied=True, firmware_verified=True,
                          last_error=0, los=fiber == 'disconnected')
        omci = dict(schema_version=1, state=1, onu_id=65535, gem_port_id=65535,
                    agent_enabled=1, agent_operational=0, authenticated=0,
                    service_rules=0, service_error=0, rx_packets='0', rx_dropped='0',
                    tx_packets='0', tx_errors='0', mib_objects=322)
        samples = []
        for n in range(count):
            samples.append(dict(rx_bench=True, tx_inhibited=True, registration_enabled=False,
                                tx_enabled=False, mac_irq_mask=0, controller_los=controller['los'],
                                phy_los=controller['los'], synced=not controller['los'],
                                sync_status=0, frames=n if fiber == 'connected' else 0,
                                lof=0, fec_total=0, fec_corrected=0, fec_uncorrected=0,
                                irq_calls=0, poll_calls=n // 2, sampled_ms=(n + 1) * 1000))
        return record, controller, omci, samples

    def save(self, record, controller, omci, samples):
        self.capture.joinpath('checkpoint.json').write_text(json.dumps(record))
        lines = [json.dumps(controller)]
        for sample in samples:
            lines += [json.dumps(controller), 'protocol_error=0', json.dumps(omci)]
            if record['action'] == 'receive':
                lines.append(json.dumps(sample))
            lines += ['fiber_led green:wan-1 brightness=0', 'fiber_led red:wan brightness=1']
        self.capture.joinpath('attempt.log').write_text('\n'.join(lines) + '\n')
        self.capture.joinpath('serial.log').write_text('')

    def test_stack_and_both_receive_states(self):
        for action, fiber in [('stack', 'disconnected'), ('receive', 'disconnected'), ('receive', 'connected')]:
            with self.subTest(action=action, fiber=fiber):
                self.save(*self.fixture(action, fiber))
                result = REPORT.summarize(self.capture)
                self.assertEqual(result['action'], action)
                self.assertTrue(result['physical_led_requires_user_observation'])
                if action == 'receive':
                    self.assertEqual(result['receive']['samples'], 30)
                    self.assertEqual(result['receive']['final_stable_intervals'], 29 if fiber == 'connected' else 0)

    def test_receive_rejects_guard_counter_and_freshness_failures(self):
        changes = [('tx_enabled', True), ('registration_enabled', True), ('tx_inhibited', False),
                   ('mac_irq_mask', 1), ('sampled_ms', 1000), ('poll_calls', 0),
                   ('phy_los', False), ('synced', True), ('frames', True), ('fec_total', -1)]
        for key, value in changes:
            with self.subTest(key=key):
                record, controller, omci, samples = self.fixture()
                samples[10][key] = value
                self.save(record, controller, omci, samples)
                with self.assertRaises(ValueError):
                    REPORT.summarize(self.capture)

    def test_connected_requires_final_five_progressing_intervals(self):
        for problem in ('no-poll', 'frozen-frames', 'lost-sync-at-end', 'counter-wrap'):
            with self.subTest(problem=problem):
                record, controller, omci, samples = self.fixture(fiber='connected')
                if problem == 'no-poll':
                    for sample in samples:
                        sample['poll_calls'] = 0
                elif problem == 'frozen-frames':
                    for sample in samples:
                        sample['frames'] = 10
                elif problem == 'lost-sync-at-end':
                    samples[-1]['synced'] = False
                else:
                    for n, sample in enumerate(samples):
                        sample['frames'] = (0xfffffff0 + n) & 0xffffffff
                self.save(record, controller, omci, samples)
                if problem == 'counter-wrap':
                    REPORT.summarize(self.capture)
                else:
                    with self.assertRaises(ValueError):
                        REPORT.summarize(self.capture)

    def test_reacquire_request_and_single_attempt_must_match(self):
        for problem in ('none', 'missing', 'repeat', 'unexpected', 'early', 'rollback'):
            with self.subTest(problem=problem):
                record, controller, omci, samples = self.fixture(fiber='connected')
                record['reacquire_once'] = problem != 'unexpected'
                for n, item in enumerate(samples):
                    item.update(reacquire_enabled=True, reacquire_attempts=int(n >= 20))
                if problem == 'missing':
                    del samples[0]['reacquire_enabled']
                elif problem == 'repeat':
                    samples[-1]['reacquire_attempts'] = 2
                elif problem == 'early':
                    samples[1]['reacquire_attempts'] = 1
                elif problem == 'rollback':
                    samples[-1]['reacquire_attempts'] = 0
                self.save(record, controller, omci, samples)
                if problem == 'none':
                    self.assertEqual(REPORT.summarize(self.capture)['receive']['reacquire_attempts'], 1)
                else:
                    with self.assertRaises(ValueError):
                        REPORT.summarize(self.capture)

    def test_pll_restoration_request_must_match_each_sample(self):
        for problem in ('none', 'missing', 'unexpected', 'without-recovery', 'wrong-type'):
            with self.subTest(problem=problem):
                record, controller, omci, samples = self.fixture(fiber='connected')
                record.update(restore_pll=problem != 'unexpected',
                              reacquire_once=problem != 'without-recovery')
                for n, item in enumerate(samples):
                    item.update(pll_restore_enabled=True, reacquire_enabled=True,
                                reacquire_attempts=int(n >= 20))
                if problem == 'missing':
                    del samples[-1]['pll_restore_enabled']
                elif problem == 'wrong-type':
                    samples[-1]['pll_restore_enabled'] = 1
                self.save(record, controller, omci, samples)
                if problem == 'none':
                    self.assertTrue(REPORT.summarize(self.capture)['receive']['pll_restore_requested'])
                else:
                    with self.assertRaises(ValueError):
                        REPORT.summarize(self.capture)

    def test_extended_observation_requires_all_requested_samples(self):
        for count in (90, 180):
            fixture = self.fixture(fiber='connected', count=count)
            self.save(*fixture)
            self.assertEqual(REPORT.summarize(self.capture)['observations'], count)
            fixture[3].pop()
            self.save(*fixture)
            with self.assertRaises(ValueError):
                REPORT.summarize(self.capture)

    def test_matrix_can_continue_only_after_isolated_downstream_failure(self):
        for problem in ('none', 'tx', 'serial', 'cleanup', 'timeout', 'unknown-error'):
            record, controller, omci, samples = self.fixture(fiber='connected')
            record.update(status='failed', error='SSH failed (1); see attempt.log')
            for sample in samples:
                sample.update(frames=0, synced=False)
            if problem == 'tx':
                samples[12]['tx_enabled'] = True
            if problem == 'cleanup':
                record['postflight'] = 'failed'
            if problem == 'timeout':
                record['error'] = 'timed out'
            self.save(record, controller, omci, samples)
            with (self.capture / 'attempt.log').open('a') as output:
                output.write('Q1000K bench: Downstream LOS/sync/frame stability was not established.\n')
                if problem == 'unknown-error':
                    output.write('Q1000K bench: another failure\n')
            if problem == 'serial':
                (self.capture / 'serial.log').write_text('Kernel panic\n')
            with self.assertRaises(ValueError):
                REPORT.summarize(self.capture)  # Never a passing report.
            if problem == 'none':
                report = REPORT.summarize(self.capture, allow_downstream_failure=True)
                self.assertEqual(report['bench_result'], 'failed')
                self.assertFalse(report['receive']['downstream_stable'])
            else:
                with self.assertRaises(ValueError):
                    REPORT.summarize(self.capture, allow_downstream_failure=True)

    def test_refuses_missing_evidence_and_kernel_or_cleanup_failures(self):
        for problem in ('missing-sample', 'missing-leds', 'serial-error', 'cleanup', 'omci-assigned'):
            with self.subTest(problem=problem):
                fixture = self.fixture()
                if problem == 'missing-sample':
                    fixture[3].pop()
                if problem == 'cleanup':
                    fixture[0]['postflight'] = 'failed'
                if problem == 'omci-assigned':
                    fixture[2]['onu_id'] = 1
                self.save(*fixture)
                if problem == 'serial-error':
                    self.capture.joinpath('serial.log').write_text('WARNING: teardown failed\n')
                if problem == 'missing-leds':
                    path = self.capture / 'attempt.log'
                    path.write_text('\n'.join(x for x in path.read_text().splitlines() if not x.startswith('fiber_led ')))
                with self.assertRaises(ValueError):
                    REPORT.summarize(self.capture)


if __name__ == '__main__':
    unittest.main()
