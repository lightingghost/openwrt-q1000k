#!/usr/bin/env python3
"""Run the production supervisor with only temporary files and fake loaders."""
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import unittest

import test_xgspon

REPO = test_xgspon.REPO
MODULES = ('q1000k_pon_control airoha_ecnt_hook airoha_ecnt_scu '
           'airoha_ecnt_pon_phy airoha_ecnt_xpon phy_10g xpon omci xpon_10g').split()


class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.backend = test_xgspon.BackendTests()
        self.backend.setUp()
        self.addCleanup(self.backend.tearDown)
        self.root, self.env, self.write = self.backend.root, self.backend.env, self.backend.write
        self.env.update(TEST_ROOT=str(self.root), TEST_ENABLED='1', TEST_LOWER='eth2',
                        TEST_REG='a1' * 36, TEST_EQUIPMENT='', TEST_VERSION='')
        self.write('factory.json', json.dumps({'available': True, 'serial': 'TEST01234567',
                                             'wan_mac': '00:11:22:33:44:55'}))
        for suffix, (data, _) in self.backend.firmware.items():
            self.write('lib/firmware/airoha/q1000k/A60993.elf.' + suffix, data.decode())
        self.write('tmp/sysinfo/board_name', 'quantum,q1000k-ubi\n')
        self.write('sys/class/net/eth2/flags', '0x1003\n')
        self.controller = self.root / 'sys/bus/i2c/drivers/q1000k-pon-control/0-0051'
        self.controller.mkdir(parents=True)
        self.status = dict(schema_version=1, mode='xgspon', firmware_verified=True,
                           calibration_supplied=True, md32_enabled=True, last_error=0,
                           tx_disabled=True)
        self.sample(self.controller / 'status', self.status)
        (self.controller / 'operation').write_text('')
        self.sample(self.root / 'omci.json', dict(schema_version=1, service_error=0))
        self.write('proc/xgpon/status', 'state=2\nprotocol_error=0\n')
        self.write('uci', '''#!/bin/sh
case "$*" in
*service.enabled) printf '%s' "$TEST_ENABLED" ;;
*service.lower) printf '%s' "$TEST_LOWER" ;;
*service.continuous_bench) printf '%s' "$TEST_CONTINUOUS" ;;
*network.*.q1000k_profile) printf '%s' "${TEST_WAN_PROFILE:-1}" ;;
*network.*.device) printf pon ;;
*identity.registration_id) printf '%s' "$TEST_REG" ;;
*identity.equipment_id) printf '%s\n' "$TEST_EQUIPMENT" ;;
*identity.omci_version) printf '%s\n' "$TEST_VERSION" ;;
*identity.hardware_version) printf '%s\n' "$TEST_HARDWARE" ;;
*identity.software_version_a) printf '%s\n' "$TEST_SOFTWARE_A" ;;
*identity.software_version_b) printf '%s\n' "$TEST_SOFTWARE_B" ;;
*identity.logical_password) printf '%s\n' "$TEST_LOGICAL_PASSWORD" ;;
*identity.active_bank) printf '%s\n' "$TEST_BANK" ;;
esac
''').chmod(0o755)
        # These absolute fixture programs replace every loader/CLI invocation;
        # even a bad fixture PATH cannot invoke a host module loader.
        fake = '''import json, os, pathlib, sys
root = pathlib.Path(os.environ['TEST_ROOT'])
action = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
if action == 'omci':
    assert args == ['-i', 'pon', 'status']
    print((root / 'omci.json').read_text())
    sys.exit(int(os.environ.get('OMCI_FAIL', '0')))
with (root / 'calls').open('a') as log:
    log.write(json.dumps([action] + args) + '\\n')
if os.environ.get('FAIL') == action + ':' + args[0]:
    sys.exit(1)
if action in ('modprobe', 'insmod'):
    args[0] = args[0].replace('-', '_')
    if args[0] == 'xpon_10g':
        # Model ubox: only insmod forwards command-line parameters.
        params = dict(arg.split('=', 1) for arg in args[1:]) if action == 'insmod' else {}
        if not {'wan_mac', 'pon_serial', 'pon_reg_id', 'pon_lower'} <= params.keys(): sys.exit(1)
    (root / 'sys/module' / args[0]).mkdir(parents=True)
    if args[0] == 'xpon_10g':
        (root / 'sys/class/net/pon').mkdir(parents=True)
elif action == 'rmmod':
    if args[0] == 'q1000k_pon_control':
        assert (root / 'sys/bus/i2c/drivers/q1000k-pon-control/0-0051/operation').read_text() == 'off\\n'
    (root / 'sys/module' / args[0]).rmdir()
elif action == 'initialize':
    assert args == ['initialize']
    assert (root / 'sys/bus/i2c/drivers/q1000k-pon-control/0-0051/operation').read_text() == 'detect\\n'
elif action == 'ip':
    assert args[:3] == ['link', 'set', 'dev'] and args[3] == 'ponraw'
    (root / 'sys/class/net/ponraw/flags').write_text('0x1003' if args[4] == 'up' else '0x1002')
elif action == 'ubus':
    assert args[0] == 'call' and args[1] in ('network.interface.wan', 'network.interface.wan6')
    assert args[2] in ('up', 'renew')
    if (root / 'block-network').exists(): sys.exit(1)
elif action == 'ifdown':
    assert args[0] in ('wan', 'wan6')
else:
    raise AssertionError(action)
'''
        for name in ('modprobe', 'insmod', 'rmmod', 'initialize', 'omci', 'ip', 'ubus', 'ifdown'):
            self.write(name, '#!' + sys.executable + '\n' + fake).chmod(0o755)
        self.write('sleep', '#!/bin/sh\nexec /bin/sleep 0.05\n').chmod(0o755)
        source = (REPO / 'package/network/utils/q1000k-xgspon-service/files/run').read_text()
        source = source.replace('/lib/q1000k-xgspon/common.sh', str(self.backend.common))
        source = re.sub(r'(?<![A-Za-z0-9])/(sys|proc|tmp/sysinfo|var/run)/',
                        lambda m: str(self.root) + '/' + m[1] + '/', source)
        source = source.replace('/usr/sbin/q1000k-xgspon', str(self.root / 'initialize'))
        source = source.replace('/usr/sbin/q1000k-omci', str(self.root / 'omci'))
        source = source.replace('/etc/q1000k-private-autostart', str(self.root / 'etc/q1000k-private-autostart'))
        source = source.replace('modprobe "$module"', '"' + str(self.root / 'modprobe') + '" "$module"')
        source = source.replace('insmod "$filename"', '"' + str(self.root / 'insmod') + '" "$filename"')
        source = source.replace('rmmod "$module"', '"' + str(self.root / 'rmmod') + '" "$module"')
        source = source.replace('sleep 5', '"' + str(self.root / 'sleep') + '" 5')
        for name in ('ip', 'ubus', 'ifdown'):
            source = re.sub(r'(?<![A-Za-z0-9_-])' + name + r'(?= (?:link|call|wan))',
                            '"' + str(self.root / name) + '"', source)
        self.script = self.write('supervisor', source)

    def sample(self, path, value):
        new = path.with_suffix('.new')
        new.write_text(json.dumps(value))
        new.replace(path)

    def continuous(self):
        self.env.update(TEST_CONTINUOUS='1', TEST_LOWER='ponraw')
        self.write('etc/q1000k-private-autostart', 'continuous-activation-v1\n')
        self.write('sys/firmware/devicetree/base/quantum,xgspon-activation-bench', '')
        self.write('sys/class/net/ponraw/flags', '0x1002\n')

    def provisioned(self, **changes):
        self.sample(self.root / 'omci.json', dict(dict(schema_version=1, service_error=0,
            state=5, authenticated=1, agent_operational=1, service_rules=2, mib_objects=40), **changes))

    def test_normal_board_uses_validated_recipe_without_bench_permission(self):
        self.env.update(TEST_LOWER='ponraw')
        self.write('sys/firmware/devicetree/base/quantum,xgspon-service', '')
        self.write('sys/class/net/ponraw/flags', '0x1002\n')
        p = self.launch()
        self.await_stage(p, 'waiting_registration')
        before = self.calls()
        self.assertEqual(before[0], ['modprobe', 'q1000k_pon_control'])
        self.assertIn(['ip', 'link', 'set', 'dev', 'ponraw', 'up'], before)
        self.assertIn(['modprobe', 'omci'], before)
        params = next(c[2:] for c in before if c[:2] == ['insmod', 'xpon_10g'])
        self.assertTrue({'rx_bench=0', 'bench_live_add=31', 'bench_initial_key_readback=1',
                         'bench_key_inline=1', 'bench_ranging_mode=1'} <= set(params))
        self.assertFalse(any(c[0] == 'ubus' for c in before))
        self.provisioned()
        self.await_stage(p, 'running')
        self.assertEqual(len([c for c in self.calls() if c[0] == 'ubus']), 4)
        self.provisioned(state=1, authenticated=0, agent_operational=0, service_rules=0)
        self.await_stage(p, 'waiting_registration')
        self.assertFalse(any(c[0] in ('rmmod', 'ifdown') for c in self.calls()))
        self.provisioned()
        self.await_stage(p, 'running')
        self.assertEqual(len([c for c in self.calls() if c[0] == 'ubus']), 8)
        p.terminate(); out, err = p.communicate(timeout=5)
        self.assertEqual(p.returncode, 0, err)
        self.assertEqual([c[1] for c in self.calls() if c[0] == 'rmmod'], MODULES[::-1])
        self.assertEqual(self.calls()[-1], ['ip', 'link', 'set', 'dev', 'ponraw', 'down'])
        self.assertNotIn(self.env['TEST_REG'], out + err)

    def test_normal_board_still_requires_enabled_service_and_complete_inputs(self):
        self.env.update(TEST_LOWER='ponraw')
        self.write('sys/firmware/devicetree/base/quantum,xgspon-service', '')
        self.write('sys/class/net/ponraw/flags', '0x1002\n')
        for key, bad in [('TEST_ENABLED', '0'), ('TEST_REG', ''), ('TEST_WAN_PROFILE', 'foreign')]:
            previous = self.env.get(key)
            self.env[key] = bad
            self.failed(); self.assertEqual(self.calls(), [])
            if previous is None:
                del self.env[key]
            else:
                self.env[key] = previous
        (self.root / 'lib/firmware/airoha/q1000k/A60993.elf.pm').unlink()
        self.failed(); self.assertEqual(self.calls(), [])

    def test_private_cold_start_dark_wait_recovery_and_owned_cleanup(self):
        self.continuous()
        p = self.launch()
        self.await_stage(p, 'waiting_registration')
        time.sleep(0.3)
        before = self.calls()
        self.assertEqual(before[0], ['insmod', 'q1000k-pon-control', 'validation_tx=1'])
        self.assertIn(['ip', 'link', 'set', 'dev', 'ponraw', 'up'], before)
        self.assertIn(['insmod', 'omci', 'bench_dot1x_oem=1'], before)
        params = next(c[2:] for c in before if c[:2] == ['insmod', 'xpon_10g'])
        self.assertTrue({'rx_bench=0', 'bench_live_add=31', 'bench_initial_key_readback=1',
                         'bench_key_inline=1', 'bench_ranging_mode=1'} <= set(params))
        self.assertFalse(any(c[0] in ('ubus', 'ifdown', 'rmmod') for c in before))
        self.provisioned()
        self.await_stage(p, 'running')
        network = [c for c in self.calls() if c[0] == 'ubus']
        self.assertEqual(len(network), 4)
        time.sleep(0.3)
        self.assertEqual([c for c in self.calls() if c[0] == 'ubus'], network)
        # Signal loss keeps the stack and DHCP clients; no timer expiry or
        # network teardown. Returning O5/provisioning triggers one renewal.
        self.provisioned(state=1, authenticated=0, agent_operational=0, service_rules=0)
        self.await_stage(p, 'waiting_registration')
        time.sleep(0.3)
        self.assertFalse(any(c[0] in ('rmmod', 'ifdown') for c in self.calls()))
        self.provisioned()
        self.await_stage(p, 'running')
        self.assertEqual(len([c for c in self.calls() if c[0] == 'ubus']), 8)
        self.assertEqual([c for c in self.calls() if c[0] in ('insmod', 'modprobe')],
                         [c for c in before if c[0] in ('insmod', 'modprobe')])
        p.terminate(); out, err = p.communicate(timeout=5)
        self.assertEqual(p.returncode, 0, err)
        self.assertEqual([c[1] for c in self.calls() if c[0] == 'rmmod'], MODULES[::-1])
        self.assertEqual(self.calls()[-1], ['ip', 'link', 'set', 'dev', 'ponraw', 'down'])
        self.assertNotIn(self.env['TEST_REG'], out + err)

    def test_private_profile_guards_before_optical_changes(self):
        self.continuous()
        for name in ('etc/q1000k-private-autostart', 'sys/firmware/devicetree/base/quantum,xgspon-activation-bench'):
            path = self.root / name
            data = path.read_bytes(); path.unlink()
            self.failed(); self.assertEqual(self.calls(), [])
            path.write_bytes(data)
        path = self.root / 'var/run/q1000k-pon-bench.lock'; path.mkdir(parents=True)
        self.failed(); self.assertEqual(self.calls(), [])
        path.rmdir()
        self.env['TEST_WAN_PROFILE'] = 'foreign'
        self.failed(); self.assertEqual(self.calls(), [])

    def test_private_network_retry_and_hardware_fault_have_distinct_lifecycles(self):
        self.continuous(); self.provisioned()
        events = '{"trace_version":1,"event":1,"result":-110}\n'
        self.write('proc/q1000k-pon-events', events)
        block = self.write('block-network', '')
        p = self.launch()
        self.await_stage(p, 'waiting_network')
        time.sleep(0.3)
        self.assertEqual(len([c for c in self.calls() if c[:2] == ['insmod', 'xpon_10g']]), 1)
        block.unlink()
        self.await_stage(p, 'running')
        self.sample(self.controller / 'status', dict(self.status, last_error=-5))
        out, err = p.communicate(timeout=5)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(self.last()['stage'], 'fault')
        faults = list((self.root / 'var/run/q1000k-xgspon').glob('fault-*'))
        self.assertEqual(len(faults), 1)
        self.assertEqual((faults[0] / 'q1000k-pon-events').read_text(), events)
        self.assertEqual((faults[0] / 'q1000k-pon-events').stat().st_mode & 0o777, 0o600)
        self.assertEqual(json.loads((faults[0] / 'cause.json').read_text())['error'], 1)
        calls = self.calls()
        first_unload = next(i for i, c in enumerate(calls) if c[0] == 'rmmod')
        self.assertEqual([c for c in calls[:first_unload] if c[0] == 'ifdown'],
                         [['ifdown', 'wan6'], ['ifdown', 'wan']])

    def calls(self):
        p = self.root / 'calls'
        return [json.loads(line) for line in p.read_text().splitlines()] if p.exists() else []

    def last(self):
        return json.loads((self.root / 'var/run/q1000k-xgspon/status.json').read_text())

    def launch(self, *args):
        p = subprocess.Popen(['busybox', 'ash', str(self.script), *args], env=self.env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        def stop():
            if p.poll() is None:
                p.terminate()
                p.communicate(timeout=5)
        self.addCleanup(stop)
        return p

    def running(self):
        p = self.launch()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if p.poll() is not None:
                self.fail('Early exit: ' + p.communicate()[1])
            try:
                if self.last()['stage'] == 'running':
                    return p
            except FileNotFoundError:
                pass
            time.sleep(0.02)
        self.fail('Supervisor never reached running')

    def failed(self):
        p = self.launch()
        out, err = p.communicate(timeout=5)
        self.assertNotEqual(p.returncode, 0, (out, err))
        if self.env['TEST_REG']:
            self.assertNotIn(self.env['TEST_REG'], out + err)
        return err

    def test_separate_presentation_and_short_registration_reach_the_loader(self):
        self.env.update(TEST_HARDWARE='BGW320-500_2.1', TEST_SOFTWARE_A='BGW320_4.27.7',
                        TEST_SOFTWARE_B='other-version', TEST_LOGICAL_PASSWORD='secret',
                        TEST_BANK='1', TEST_REG='0123')
        p = self.running()
        p.terminate()
        out, err = p.communicate(timeout=5)
        self.assertEqual(p.returncode, 0, err)
        args = next(c[2:] for c in self.calls() if c[:2] == ['insmod', 'xpon_10g'])
        params = dict(arg.split('=', 1) for arg in args)
        self.assertEqual(params['pon_reg_id'], '0123' + '00' * 34)
        for key, value in [('hardware_version', 'BGW320-500_2.1'), ('software0', 'BGW320_4.27.7'),
                           ('software1', 'other-version'), ('logical_password', 'secret')]:
            self.assertEqual(params['pon_' + key + '_hex'], value.encode().hex())
        self.assertEqual(params['pon_active_bank'], '1')
        self.assertNotIn('secret', out + err)
        self.assertEqual([c[1] for c in self.calls() if c[0] == 'rmmod'], MODULES[::-1])

    def test_preflight_never_loads_modules(self):
        for key, bad in [('TEST_ENABLED', '0'), ('TEST_LOWER', ''), ('TEST_LOWER', '../eth2'),
                         ('TEST_LOWER', 'eth2\neth3'), ('TEST_LOWER', 'doesnotexist'),
                         ('TEST_REG', ''), ('TEST_REG', '0' * 71), ('TEST_REG', '0' * 73),
                         ('TEST_EQUIPMENT', 'X' * 21), ('TEST_VERSION', 'X' * 15),
                         ('TEST_EQUIPMENT', 'X\nY'), ('TEST_EQUIPMENT', 'X\n'),
                         ('TEST_VERSION', 'X\r'), ('TEST_VERSION', 'X\x7f'),
                         ('TEST_EQUIPMENT', 'é'),
                         ('TEST_REG', '0' * 35 + '\n' + '0' * 36), ('TEST_REG', 'g' + '0' * 71)]:
            with self.subTest(key=key, bad=bad):
                old = self.env[key]
                self.env[key] = bad
                self.failed()
                self.assertEqual(self.calls(), [])
                self.env[key] = old
        for file, bad in [('tmp/sysinfo/board_name', 'other'), ('sys/class/net/eth2/flags', '0x1002'),
                          ('sys/class/net/eth2/flags', '0xfffffffff'),
                          ('factory.json', '{"available":false}'),
                          ('lib/firmware/airoha/q1000k/A60993.elf.dm', 'bad')]:
            with self.subTest(file=file):
                p = self.root / file
                old = p.read_text()
                p.write_text(bad)
                self.failed()
                self.assertEqual(self.calls(), [])
                p.write_text(old)
        for name in MODULES:
            p = self.root / 'sys/module' / name
            p.mkdir(parents=True)
            self.failed()
            self.assertEqual(self.calls(), [])
            p.rmdir()

    def test_exact_order_identity_and_reverse_shutdown(self):
        p = self.running()
        p.send_signal(signal.SIGTERM)
        out, err = p.communicate(timeout=5)
        self.assertEqual(p.returncode, 0, err)
        calls = self.calls()
        loads = [c for c in calls if c[0] in ('modprobe', 'insmod')]
        self.assertEqual([c[1] for c in loads], MODULES)
        self.assertEqual(calls[1], ['initialize', 'initialize'])
        self.assertEqual(loads[-1][0], 'insmod')
        self.assertEqual(loads[-1][2:], ['wan_mac=00:11:22:33:44:55', 'pon_serial=TEST01234567',
                                        'pon_reg_id=' + self.env['TEST_REG'], 'pon_lower=eth2'])
        self.assertEqual([c[1] for c in calls if c[0] == 'rmmod'], list(reversed(MODULES)))
        self.assertEqual(self.last(), dict(schema_version=1, stage='stopped', error=0))
        self.assertFalse((self.root / 'var/run/q1000k-xgspon/lock').exists())
        self.assertEqual((self.root / 'var/run/q1000k-xgspon/status.json').stat().st_mode & 0o777, 0o600)

    def await_stage(self, process, stage):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if process.poll() is not None:
                self.fail('Early monitor exit: ' + process.communicate()[1])
            try:
                if self.last()['stage'] == stage:
                    return
            except (FileNotFoundError, json.JSONDecodeError):
                pass
            time.sleep(0.02)
        self.fail('Monitor never reached ' + stage)

    def test_monitor_waits_for_inputs_without_subscriber_identity_or_tx(self):
        self.env['TEST_ENABLED'] = '0'
        self.write('factory.json', '{"available":false}')
        pm = self.root / 'lib/firmware/airoha/q1000k/A60993.elf.pm'
        saved = pm.read_bytes(); pm.unlink()
        p = self.launch('--monitor')
        self.await_stage(p, 'waiting_firmware')
        self.assertEqual(self.calls(), [['modprobe', 'q1000k_pon_control']])
        pm.write_bytes(saved)
        self.await_stage(p, 'waiting_calibration')
        self.assertEqual(self.calls(), [['modprobe', 'q1000k_pon_control']])
        # The fixture factory's calibration operation models a staged record.
        self.write('calibration.bin', 'unit calibration')
        self.await_stage(p, 'monitoring')
        self.assertEqual(self.calls(), [['modprobe', 'q1000k_pon_control'], ['initialize', 'initialize']])
        self.assertTrue((self.root / 'var/run/q1000k-xgspon/monitor').exists())
        p.terminate(); p.communicate(timeout=5)
        self.assertEqual(p.returncode, 0)
        self.assertEqual(self.calls()[-1], ['rmmod', 'q1000k_pon_control'])
        self.assertFalse((self.root / 'var/run/q1000k-xgspon/monitor').exists())

    def test_monitor_refuses_existing_bench_or_module_ownership(self):
        for name in ('var/run/q1000k-pon-bench.lock', 'sys/module/q1000k_pon_control'):
            with self.subTest(name=name):
                path = self.root / name; path.mkdir(parents=True)
                p = self.launch('--monitor'); p.communicate(timeout=5)
                self.assertNotEqual(p.returncode, 0)
                self.assertEqual(self.calls(), [])
                self.assertTrue(path.exists()); path.rmdir()

    def test_monitor_fault_releases_controller(self):
        p = self.launch('--monitor')
        self.await_stage(p, 'monitoring')
        self.sample(self.controller / 'status', dict(self.status, tx_disabled=False))
        p.communicate(timeout=5)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(self.calls()[-1], ['rmmod', 'q1000k_pon_control'])

    def test_omci_overrides_are_encoded_before_module_loading(self):
        equipment = "Q \"'`$()\\;=".ljust(20, '.')
        version = 'TEST-version'.ljust(14, ' ')
        self.env.update(TEST_EQUIPMENT=equipment, TEST_VERSION=version)
        p = self.running()
        p.terminate()
        out, err = p.communicate(timeout=5)
        self.assertEqual(p.returncode, 0, err)
        args = [c for c in self.calls() if c[:2] == ['insmod', 'xpon_10g']][0]
        self.assertEqual(args[-2:], ['pon_equipment_id_hex=' + equipment.encode().hex(),
                                    'pon_omci_version_hex=' + version.encode().hex()])
        self.assertNotIn(equipment, out + err)
        self.assertNotIn(version, out + err)

    def test_every_load_and_initialize_failure_releases_only_owned_modules(self):
        for i, name in enumerate(MODULES):
            with self.subTest(name=name):
                self.env['FAIL'] = ('insmod:' if name == 'xpon_10g' else 'modprobe:') + name
                self.failed()
                self.assertEqual([c[1] for c in self.calls() if c[0] == 'rmmod'],
                                 list(reversed(MODULES[:i])))
                (self.root / 'calls').unlink()
                self.assertEqual(list((self.root / 'sys/module').iterdir()) if i else [], [])
        self.env['FAIL'] = 'initialize:initialize'
        self.failed()
        self.assertEqual(self.calls(), [['modprobe', MODULES[0]], ['initialize', 'initialize'],
                                       ['rmmod', MODULES[0]]])

    def test_failed_unload_retains_dependencies_and_reports_error(self):
        self.env['FAIL'] = 'rmmod:xpon_10g'
        p = self.running()
        p.terminate()
        p.communicate(timeout=5)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual([c for c in self.calls() if c[0] == 'rmmod'], [['rmmod', 'xpon_10g']])
        self.assertTrue(all((self.root / 'sys/module' / n).is_dir() for n in MODULES))
        self.assertEqual(self.last()['stage'], 'cleanup_failed')

    def test_fault_closes_lifecycle_without_respawn(self):
        for source, bad in [('protocol', 'state=5\nprotocol_error=-5\n'),
                            ('omci', dict(schema_version=1, service_error=-117)),
                            ('omci', dict(schema_version=1, service_error='0')),
                            ('omci', dict(schema_version=2, service_error=0)),
                            ('controller', dict(self.status, md32_enabled='true')),
                            ('controller', dict(self.status, last_error=-5))]:
            with self.subTest(source=source, bad=bad):
                p = self.running()
                path = {'protocol': self.root / 'proc/xgpon/status', 'omci': self.root / 'omci.json',
                        'controller': self.controller / 'status'}[source]
                old = path.read_text()
                path.write_text(bad if isinstance(bad, str) else json.dumps(bad))
                p.communicate(timeout=5)
                self.assertNotEqual(p.returncode, 0)
                self.assertEqual(self.last()['stage'], 'fault')
                self.assertEqual([c[1] for c in self.calls() if c[0] == 'rmmod'], list(reversed(MODULES)))
                path.write_text(old)
                (self.root / 'calls').unlink()
                (self.root / 'sys/class/net/pon').rmdir()

    def test_incomplete_provisioning_does_not_power_cycle(self):
        self.sample(self.root / 'omci.json', dict(schema_version=1, service_error=-95))
        p = self.running()
        time.sleep(0.2)
        self.assertIsNone(p.poll())
        p.terminate()
        p.communicate(timeout=5)
        self.assertEqual(p.returncode, 0)

    def test_second_instance_cannot_adopt_or_unload_first(self):
        first = self.running()
        before = self.calls()
        self.failed()
        self.assertEqual(self.calls(), before)
        self.assertIsNone(first.poll())
        self.assertEqual(self.last()['stage'], 'running')
        first.terminate()
        first.communicate(timeout=5)
        self.assertEqual(first.returncode, 0)

    def test_controller_verification_failure_precedes_phy_load(self):
        for bad in (dict(self.status, tx_disabled=False), dict(self.status, schema_version='1'),
                    dict(self.status, firmware_verified=False)):
            self.sample(self.controller / 'status', bad)
            self.failed()
            self.assertEqual(self.calls(), [['modprobe', MODULES[0]], ['initialize', 'initialize'],
                                           ['rmmod', MODULES[0]]])
            (self.root / 'calls').unlink()

    def test_cli_dispatch_and_last_report_do_not_claim_optical_service(self):
        self.write('service-init', '#!/bin/sh\nprintf "%s\\n" "$*"\n').chmod(0o755)
        for action in ('start', 'stop', 'restart', 'reload'):
            r = subprocess.run(['busybox', 'ash', str(self.backend.cli), action],
                               env=self.env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout, action + '\n')
        self.write('run/status.json', json.dumps(dict(schema_version=1, stage='running', error=0)))
        _, data = self.backend.call()
        self.assertEqual(data['supervisor'], dict(available=True, enabled=True, monitor_enabled=True, last_stage='running', last_error=0))
        self.assertTrue(data['activation_supported'])
        self.assertIsNone(data['service_ready'])
        self.write('run/status.json', '{"schema_version":"1","stage":"running","error":0}')
        self.assertIsNone(self.backend.call()[1]['supervisor']['last_stage'])


if __name__ == '__main__':
    unittest.main()
