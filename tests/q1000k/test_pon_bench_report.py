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

    def fixture(self, action='receive', fiber='disconnected'):
        count = 30 if action == 'receive' else 5
        record = dict(schema_version=1, action=action, fiber=fiber, host='192.168.255.1',
                      status='passed', postflight='passed', input_cleanup='passed',
                      revision='a' * 40, serial_start=0, started=1, finished=50)
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
