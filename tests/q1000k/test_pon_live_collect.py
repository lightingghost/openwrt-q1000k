#!/usr/bin/env python3
"""Live observation retains service ownership on success, failure and interruption."""
import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('live_collect', ROOT / 'scripts/q1000k/live-collect.py')
LIVE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LIVE)


def snapshot(full=True, pid=123, boot='12345678-1234-1234-1234-123456789abc', los=False, malformed=False):
    data = dict(board='quantum,q1000k-ubi', boot_id=boot, uptime='120.00 110.00', build_info='fixture revision',
                status=dict(schema_version=1, modules=dict(phy_loaded=full, mac_loaded=full),
                            controller=dict(available=True, tx_inhibited=not full),
                            supervisor=dict(last_stage='running' if full else 'monitoring'),
                            registration=5 if full else None, los=los,
                            identity=dict(serial='PRIVATE-SERIAL', registration_id='PRIVATE-CREDENTIAL'),
                            factory=dict(serial='PRIVATE-FACTORY'), future_secret='PRIVATE-FUTURE'),
                supervisor={'q1000k-xgspon': {'instances': {'instance1': dict(running=True, pid=pid, command=['PRIVATE-ARG'])}}},
                pon_link=[dict(ifname='pon', stats64=dict(rx=dict(packets=2**63 + 1)))],
                ponraw_link=[], addresses=[], routes4=[], routes6=[], wan4=dict(up=full), wan6=dict(up=full))
    if malformed:
        data['status'] = 'PRIVATE-MALFORMED'
    rows = [dict(name=name, returncode=0, data=value if name in LIVE.TEXT_FIELDS else json.dumps(value))
            for name, value in data.items()]
    return dict(returncode=0, stdout='\n'.join(json.dumps(row) for row in rows), stderr='')


class Remote:
    def __init__(self, responses):
        self.responses = iter(responses)
        self.calls = []

    def run(self, script, timeout=30):
        self.calls.append((script, timeout))
        value = next(self.responses)
        if isinstance(value, BaseException):
            raise value
        return value


def arguments(output, probes=(), samples=1):
    return argparse.Namespace(output=output, host='192.168.255.1', samples=samples, interval=5, probe_ip=list(probes))


class LiveCollectionTests(unittest.TestCase):
    def test_running_stack_observation_does_not_take_lifecycle_or_expose_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'evidence'
            remote = Remote([snapshot(), snapshot()])
            code, record = LIVE.collect(arguments(out), remote)
            self.assertEqual(code, 0)
            self.assertEqual(record['initial_state']['mode'], 'full-stack')
            self.assertTrue(record['same_lifecycle_at_end'])
            self.assertFalse(record['service_functionality_verified'])
            self.assertEqual([script for script, _ in remote.calls], [LIVE.SNAPSHOT_SCRIPT] * 2)
            self.assertNotIn('PRIVATE-', ''.join(p.read_text() for p in out.glob('*.json')))
            self.assertIn(str(2**63 + 1), (out / 'final.json').read_text())
            self.assertEqual(out.stat().st_mode & 0o777, 0o700)
            self.assertEqual((out / 'final.json').stat().st_mode & 0o777, 0o600)

    def test_monitor_only_stack_is_observed_without_loading_phy_or_mac(self):
        with tempfile.TemporaryDirectory() as directory:
            remote = Remote([snapshot(False), snapshot(False), snapshot(False)])
            code, record = LIVE.collect(arguments(Path(directory) / 'out', ['1.1.1.1']), remote)
            self.assertEqual(code, 0)
            self.assertEqual(record['initial_state']['mode'], 'controller-only')
            self.assertEqual(record['probes'][0]['status'], 'skipped')
            self.assertFalse(record['probes'][0]['traffic_sent'])
            self.assertTrue(all(command == LIVE.SNAPSHOT_SCRIPT for command, _ in remote.calls))

    def test_ping_is_bounded_and_bound_to_pon_after_route_check(self):
        for target, family in [('1.1.1.1', '-4'), ('2606:4700:4700::1111', '-6')]:
            with self.subTest(target=target), tempfile.TemporaryDirectory() as directory:
                route = dict(returncode=0, stdout='[{"dev":"pon"}]', stderr='')
                ping = dict(returncode=0, stdout='3 received', stderr='')
                remote = Remote([snapshot(), snapshot(), route, ping, snapshot()])
                code, record = LIVE.collect(arguments(Path(directory) / 'out', [target]), remote)
                self.assertEqual(code, 0)
                self.assertEqual(record['probes'][0]['status'], 'passed')
                self.assertIn(f'ip {family} -j route get {target} oif pon', remote.calls[2][0])
                self.assertIn(f'ping {family} -I pon -c 3 -W 2 -w 10 {target}', remote.calls[3][0])
                self.assertFalse(record['service_functionality_verified'])

    def test_route_through_management_cannot_pass_a_pon_probe(self):
        for body in ('[{"dev":"br-lan"}]', '[]', '{"dev":"pon"}', 'invalid'):
            with self.subTest(body=body):
                remote = Remote([dict(returncode=0, stdout=body, stderr='')])
                record = LIVE.probe(remote, LIVE.probe_address('1.1.1.1'), LIVE.parse_snapshot(snapshot()))
                self.assertEqual(record['status'], 'skipped')
                self.assertFalse(record['traffic_sent'])
                self.assertEqual(len(remote.calls), 1)

    def test_external_restart_reboot_or_module_change_stops_without_cleanup(self):
        for changed in (snapshot(pid=456), snapshot(boot='abcdef01-1234-1234-1234-123456789abc'), snapshot(full=False)):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as directory:
                out = Path(directory) / 'out'
                remote = Remote([snapshot(), changed])
                code, record = LIVE.collect(arguments(out, samples=2), remote, sleep=lambda _: None)
                self.assertEqual(code, 3)
                self.assertEqual(record['status'], 'lifecycle-changed')
                self.assertTrue((out / 'sample-0001.json').exists())
                self.assertTrue(all(command == LIVE.SNAPSHOT_SCRIPT for command, _ in remote.calls))

    def test_signal_loss_is_recorded_without_stopping_or_restarting_service(self):
        with tempfile.TemporaryDirectory() as directory:
            remote = Remote([snapshot(), snapshot(los=True), snapshot(los=False)])
            code, record = LIVE.collect(arguments(Path(directory) / 'out', samples=2), remote, sleep=lambda _: None)
            self.assertEqual(code, 0)
            self.assertTrue(record['same_lifecycle_at_end'])
            self.assertEqual(len(remote.calls), 3)

    def test_interrupt_and_timeout_preserve_partial_evidence_and_never_cleanup_remote(self):
        for fault, expected in ((KeyboardInterrupt(), 130), (subprocess.TimeoutExpired('ssh', 30), 1)):
            with self.subTest(fault=type(fault).__name__), tempfile.TemporaryDirectory() as directory:
                out = Path(directory) / 'out'
                remote = Remote([snapshot(), fault])
                code, record = LIVE.collect(arguments(out, samples=2), remote, sleep=lambda _: None)
                self.assertEqual(code, expected)
                self.assertTrue((out / 'sample-0000.json').exists())
                self.assertTrue((out / 'sha256sums').exists())
                self.assertEqual(len(remote.calls), 2)
                self.assertFalse(record['lifecycle_control'])

    def test_snapshot_rejects_wrong_schema_or_incomplete_data_without_exposing_raw_response(self):
        for response in (snapshot(malformed=True), dict(returncode=0, stdout='PRIVATE-RAW', stderr='')):
            with self.assertRaises(LIVE.CollectionError) as raised:
                LIVE.parse_snapshot(response)
            self.assertNotIn('PRIVATE-', str(raised.exception))

    def test_existing_evidence_is_not_overwritten_and_router_is_not_contacted(self):
        with tempfile.TemporaryDirectory() as directory:
            remote = Remote([])
            with self.assertRaises(FileExistsError):
                LIVE.collect(arguments(Path(directory)), remote)
            self.assertEqual(remote.calls, [])

    def test_cli_plan_is_offline_and_rejects_command_injection(self):
        with patch.object(LIVE, 'Remote') as remote, contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(LIVE.main(['--dry-run']), 0)
            self.assertFalse(json.loads(output.getvalue())['lifecycle_control'])
            remote.assert_not_called()
        for flag, value in [('--host', 'router; reboot'), ('--probe-ip', '1.1.1.1; reboot'),
                            ('--probe-ip', '127.0.0.1'), ('--samples', '0'), ('--interval', '1')]:
            with self.subTest(flag=flag, value=value), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                LIVE.main(['--dry-run', flag, value])


if __name__ == '__main__':
    unittest.main()
