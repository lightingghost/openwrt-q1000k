#!/usr/bin/env python3
"""Exercise the staged CLI, LuCI validators and encoded loader arguments without hardware."""
import json
from pathlib import Path
import subprocess
import sys
import unittest
import test_xgspon
import test_pon_wan

ROOT = test_xgspon.REPO
PACKAGE = ROOT / 'package/network/utils/q1000k-xgspon'

class ConfigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_pon_wan.WanTests.setUpClass.__func__(cls)

    def setUp(self):
        self.backend = test_xgspon.BackendTests()
        self.backend.setUp()
        self.addCleanup(self.backend.tearDown)
        self.root, self.env = self.backend.root, self.backend.env
        for directory in ('config', 'delta', 'overrides'):
            (self.root / directory).mkdir()
        (self.root / 'config/xgspon').write_text((PACKAGE / 'files/xgspon.config').read_text())
        self.env.update(PON_TEST_ROOT=str(self.root), PON_TEST_UCI=str(self.uci))
        self.backend.write('uci', '#!' + sys.executable + '\n' + """import os, pathlib, sys
root = pathlib.Path(os.environ['PON_TEST_ROOT'])
uci = os.environ['PON_TEST_UCI']
os.execv(uci, [uci, '-c', str(root/'config'), '-C', str(root/'overrides'),
              '-t', str(root/'delta'), *sys.argv[1:]])
""").chmod(0o755)
        self.cli = self.backend.write('config-cli', (PACKAGE / 'files/omci-config').read_text().replace(
            '/lib/q1000k-xgspon/common.sh', str(self.backend.common)))

    def call(self, *args):
        return subprocess.run(['busybox', 'ash', str(self.cli), *args], env=self.env,
                              text=True, capture_output=True, timeout=10)

    def test_omci_config_set_applies_after_commit_when_service_is_installed(self):
        service = self.backend.write('service-init', '#!/bin/sh\nexit 0\n')
        service.chmod(0o755)
        applied = self.root / 'applied'
        command = self.backend.write('apply-command',
                                     '#!/bin/sh\nprintf "applied\\n" > "' + str(applied) + '"\n')
        command.chmod(0o755)
        source = (PACKAGE / 'files/omci-config').read_text()
        source = source.replace('/lib/q1000k-xgspon/common.sh', str(self.backend.common))
        source = source.replace('/etc/init.d/xgspon', str(service))
        source = source.replace('/usr/sbin/reload_xgspon_config', str(command))
        self.cli = self.backend.write('config-cli', source)
        result = self.call('set', 'equipment_id', 'new-value')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(applied.read_text(), 'applied\n')
        self.assertIn("option equipment_id 'new-value'", (self.root / 'config/xgspon').read_text())

    def test_all_settings_roundtrip_parameters_and_redaction(self):
        values = dict(serial='HUMA12345678', vendor_id='TEST', equipment_id='iONT320500X',
                      hardware_version='BGW320-500_2.1', sync_circuit_pack='1',
                      software_version_a='BGW320_4.27.7', software_version_b='other-version',
                      active_bank='1', committed_bank='0', registration_id='0123',
                      logical_onu_id='logical-identity24-bytes', logical_password='password-12!',
                      wan_mac='02:00:00:00:00:01', mib_profile='native-pptp', fix_vlans='1',
                      omcc_version='0xA3', pon_slot='3', olt_profile='nokia',
                      iphost_mac='02:11:22:33:44:55', iphost_hostname='gateway', iphost_domain='example.test')
        for key, value in values.items():
            result = self.call('set', key, value)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self.call('get', key).stdout, value + '\n')
        self.assertTrue(json.loads(self.call('validate').stdout)['valid'])
        listing = json.loads(self.call('list').stdout)
        for key in ('registration_id', 'logical_onu_id', 'logical_password'):
            self.assertEqual(listing[key], '[redacted]')
        shell = '. "$1"; identity_options || exit; printf "%s\\n%s\\n" "$identity_params" "$registration"'
        result = subprocess.run(['busybox', 'ash', '-c', shell, 'fixture', str(self.backend.common)],
                                env=self.env, text=True, capture_output=True, check=True)
        params, reg = result.stdout.splitlines()
        params = dict(word.split('=', 1) for word in params.split())
        self.assertEqual(reg, '0123' + '00' * 34)
        for key, parameter in [('vendor_id', 'vendor_id'), ('equipment_id', 'equipment_id'),
                               ('hardware_version', 'hardware_version'), ('software_version_a', 'software0'),
                               ('software_version_b', 'software1'), ('logical_onu_id', 'logical_onu_id'),
                               ('logical_password', 'logical_password'), ('iphost_hostname', 'iphost_hostname'),
                               ('iphost_domain', 'iphost_domain')]:
            self.assertEqual(params['pon_' + parameter + '_hex'], values[key].encode().hex())
        self.assertEqual(params['pon_fix_vlans'], '1')
        self.assertEqual(params['pon_uni_slot'], '3')
        self.assertEqual(params['pon_omcc_version'], '0xA3')
        self.assertEqual(params['pon_iphost_mac'], values['iphost_mac'])
        self.assertEqual(params['pon_olt_profile'], '3')
        self.assertNotIn('pon_serial', params)
        self.assertNotIn('pon_reg_id', params)
        self.assertEqual(self.call('clear', 'logical_password').returncode, 0)
        self.assertEqual(self.call('get', 'logical_password').stdout, '\n')

    def test_validation_shared_with_luci_and_uci_injection_rejected(self):
        cases = []
        for key, limit in [('equipment_id', 20), ('hardware_version', 14), ('software_version_a', 14),
                           ('software_version_b', 14), ('logical_onu_id', 24), ('logical_password', 12),
                           ('iphost_hostname', 25), ('iphost_domain', 25)]:
            for value, valid in [('', True), ('x' * limit, True), ('x' * (limit + 1), False),
                                  ('quote\'"$();', True), ('bad\nvalue', False), ('end\n', False),
                                  ('\t', False), ('é', False), ('x\x7f', False)]:
                cases.append([key, value, valid])
        cases += [['serial', v, ok] for v, ok in [('HUMA12345678', True), ('HUMA12345678\n', False), ('HUMA...', False)]]
        cases += [['vendor_id', v, ok] for v, ok in [('HUMA', True), ('HUM', False), ('HUMA\n', False), ('HU-A', False)]]
        cases += [['registration_id', v, ok] for v, ok in [('00', True), ('a1' * 36, True), ('a1' * 37, False), ('1', False), ('0x00', False), ('00\n', False)]]
        cases += [['omcc_version', v, ok] for v, ok in [('0x80', True), ('0xbf', True), ('0xA3', True), ('0x7f', False), ('0xc0', False), ('163', False), ('0xA3\n', False)]]
        cases += [['pon_slot', v, ok] for v, ok in [('1', True), ('254', True), ('128', False), ('255', False), ('0', False), ('03', False), ('1\n', False), ('1.0', False)]]
        cases += [['iphost_mac', v, ok] for v, ok in [('02:11:22:33:44:55', True), ('01:11:22:33:44:55', False), ('00:00:00:00:00:00', False), ('02:11:22:33:44:55\n', False)]]
        for key, value, valid in cases:
            with self.subTest(key=key, value=value):
                result = self.call('set', key, value)
                self.assertEqual(result.returncode == 0, valid, result.stderr)
        result = subprocess.run(['node', str(ROOT / 'tests/q1000k/luci_pon_settings_fixture.js'),
                                 str(ROOT / 'package/luci-app-econet-xpon/htdocs/luci-static/resources/view/econet-xpon/settings.js')],
                                input=json.dumps(cases), text=True, capture_output=True, check=True)
        self.assertEqual(json.loads(result.stdout), [case[2] for case in cases])
        for key, value in [('active_bank', '2'), ('sync_circuit_pack', '-1'), ('fix_vlans', 'yes'),
                           ('mib_profile', '/etc/mibs/prx300_1U.ini'), ('service.enabled', '1'),
                           ('serial; touch /tmp/invalid', 'a')]:
            self.assertNotEqual(self.call('set', key, value).returncode, 0)
        self.assertNotEqual(self.call('set', 'serial', 'TEST00000001', 'extra').returncode, 0)
        # Out-of-band malformed UCI is rejected before a launcher sees parameters.
        subprocess.run([str(self.root / 'uci'), '-q', 'set', 'xgspon.identity.logical_password=secret\n'],
                       env=self.env, check=True)
        result = self.call('validate')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('secret', result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
