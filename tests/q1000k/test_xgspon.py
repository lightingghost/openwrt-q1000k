#!/usr/bin/env python3
"""Run the production factory reader and shell backend against synthetic data."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True

REPO = Path(__file__).resolve().parents[2]
PACKAGE = REPO / 'package/network/utils/q1000k-xgspon'


class FactoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='q1000k-factory-test-')
        cls.reader = Path(cls.build.name) / 'reader'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-O2', str(PACKAGE / 'src/factory.c'),
                        '-o', str(cls.reader)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='q1000k-factory-data-')
        self.root = Path(self.tmp.name)
        self.image = self.root / 'factory.bin'
        self.data = bytearray(65536)
        self.data[0x5000:0x5006] = bytes.fromhex('001122334455')
        self.data[0x9000:0x900d] = b'TEST01234567\0'
        self.cal = bytes(i % 251 for i in range(513))
        self.data[0xb000:0xb201] = self.cal

    def tearDown(self):
        self.tmp.cleanup()

    def run_reader(self, action='inspect', image=None, dsd=False):
        if image is None:
            self.image.write_bytes(self.data)
            image = self.image
        before = hashlib.sha256(image.read_bytes()).digest()
        result = subprocess.run([str(self.reader), action, '--dsd-file' if dsd else '--factory-file', str(image)],
                                capture_output=True)
        self.assertEqual(before, hashlib.sha256(image.read_bytes()).digest())
        return result

    def test_valid_identity_and_exact_calibration(self):
        r = self.run_reader()
        self.assertEqual(r.returncode, 0)
        d = json.loads(r.stdout)
        self.assertEqual(d['serial'], 'TEST01234567')
        self.assertEqual(d['wan_mac'], '00:11:22:33:44:55')
        self.assertEqual(d['calibration_bytes'], 513)
        self.assertEqual(self.run_reader('calibration').stdout, self.cal)

    def test_reject_bad_serial_mac_and_empty_payload(self):
        for offset, payload in [(0x9000, b'bad!01234567\0'), (0x9000, b'TEST01234xyz\0'),
                                (0x900c, b'!'), (0x5000, bytes.fromhex('011122334455')),
                                (0x5000, bytes(6)), (0xb000, bytes(512)),
                                (0xb000, bytes([255]) * 512)]:
            with self.subTest(offset=offset, payload=payload[:12]):
                old = self.data[:]
                self.data[offset:offset + len(payload)] = payload
                r = self.run_reader()
                self.assertNotEqual(r.returncode, 0)
                self.assertFalse(json.loads(r.stdout)['available'])
                self.assertEqual(self.run_reader('calibration').stdout, b'')
                self.data = old

    def test_short_and_symlink_input(self):
        self.data = self.data[:0xb200]
        self.assertNotEqual(self.run_reader().returncode, 0)
        link = self.root / 'link'
        link.symlink_to(self.image)
        self.assertNotEqual(self.run_reader(image=link).returncode, 0)

    def test_dsd_image_and_duplicate_fields(self):
        dsd = self.root / 'dsd.bin'
        data = bytearray([255]) * 0x12201
        for header, valid in [(b'fsan=TEST01234567\r\nwan_mac=00:11:22:33:44:55\n\0', True),
                              (b'fsan=TEST01234567\nfsan=TEST87654321\nwan_mac=00:11:22:33:44:55\n\0', False)]:
            data[:0x4000] = bytes([255]) * 0x4000
            data[:len(header)] = header
            data[0x12000:0x12201] = self.cal
            dsd.write_bytes(data)
            r = self.run_reader(image=dsd, dsd=True)
            self.assertEqual(r.returncode == 0, valid)
            if valid:
                self.assertEqual(self.run_reader('calibration', image=dsd, dsd=True).stdout, self.cal)


class BackendTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='q1000k-xgspon-backend-')
        self.root = Path(self.tmp.name)
        source = (PACKAGE / 'files/common.sh').read_text()
        source = source.replace('/usr/share/libubox/jshn.sh', str(REPO / 'staging_dir/host/share/libubox/jshn.sh'))
        source = re.sub(r'(?<![A-Za-z0-9])/(sys|proc|lib/firmware)/',
                        lambda m: str(self.root) + '/' + m[1] + '/', source)
        source = source.replace('/usr/sbin/q1000k-pon-factory', str(self.root / 'factory'))
        source = source.replace('/usr/sbin/q1000k-omci', str(self.root / 'omci'))
        source = source.replace('/etc/init.d/q1000k-xgspon', str(self.root / 'service-init'))
        source = source.replace('/var/run/q1000k-xgspon/', str(self.root / 'run') + '/')
        self.firmware = {
            'pm': (b'fixture program', '5a8a4bbae5f70c1e615ba0aa1c2a1dce654611d3205d2fa983bf41e6cdadb4a1'),
            'dm': (b'fixture data', '21618dc3694a1e6f6b28c7da7141964dea1d6e57f2d2956bbe72a780ca6166a4'),
        }
        for data, digest in self.firmware.values():
            source = source.replace(digest, hashlib.sha256(data).hexdigest())
        self.common = self.root / 'common.sh'
        self.common.write_text(source)
        self.env = dict(os.environ, PATH=f'{self.root}:{REPO}/staging_dir/host/bin:{os.environ["PATH"]}')
        self.write('proc/uptime', '123.25 20.00\n')
        self.write('factory', '#!/bin/sh\ncase "$1" in\ninspect) cat "' + str(self.root / 'factory.json') +
                   '" ;;\ncalibration) cat "' + str(self.root / 'calibration.bin') +
                   '"; [ "$CAL_FAIL" != 1 ] ;;\nesac\n').chmod(0o755)
        self.write('uci', '#!/bin/sh\ncase "$*" in\n*identity.serial) printf "%s" "$TEST_SN" ;;\n*identity.wan_mac) printf "%s" "$TEST_MAC" ;;\nesac\n').chmod(0o755)
        self.write('factory.json', json.dumps({'available': False}))
        cli = (PACKAGE / 'files/q1000k-xgspon').read_text()
        cli = cli.replace('/lib/q1000k-xgspon/common.sh', str(self.common))
        cli = cli.replace('/etc/init.d/q1000k-xgspon', str(self.root / 'service-init'))
        cli = cli.replace('/usr/sbin/q1000k-pon-factory', str(self.root / 'factory'))
        cli = cli.replace('/tmp/q1000k-xgspon.', str(self.root / 'stage/q1000k-xgspon.'))
        self.cli = self.write('cli', cli)
        (self.root / 'stage').mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, data):
        p = self.root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(data)
        return p

    def call(self, method='status'):
        r = subprocess.run(['busybox', 'ash', '-c', '. "$1"; xgspon_' + method, 'ash', str(self.common)],
                           capture_output=True, text=True, env=self.env)
        return r, json.loads(r.stdout)

    def test_absent_hardware_is_unknown(self):
        r, d = self.call()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(d['activation_supported'])
        self.assertFalse(d['identity']['valid'])
        for field in ('los', 'registration', 'omci', 'service_ready', 'optical'):
            self.assertIsNone(d[field])
        self.assertFalse(d['firmware']['program_verified'])

    def test_modules_do_not_prove_service(self):
        (self.root / 'sys/module/phy_10g').mkdir(parents=True)
        (self.root / 'sys/module/xpon_10g').mkdir(parents=True)
        _, d = self.call()
        self.assertTrue(d['modules']['mac_loaded'])
        self.assertIsNone(d['los'])
        self.assertIsNone(d['omci'])
        self.assertIsNone(d['service_ready'])

    def test_controller_status_and_unavailable_samples(self):
        base = 'sys/bus/i2c/drivers/q1000k-pon-control/0-0051/'
        status = {'schema_version': 1, 'mode': 'xgspon', 'stage': 'initialized',
                  'gpon_detected': True, 'xgspon_detected': True,
                  'checked_uptime': 100, 'md32_enabled': True, 'tx_disabled': True,
                  'firmware_verified': True, 'calibration_supplied': True,
                  'los': True, 'last_error': 0}
        self.write(base + 'operation', '')
        sample = self.write(base + 'status', json.dumps(status))
        _, d = self.call()
        self.assertTrue(d['controller']['available'])
        self.assertTrue(d['controller']['xgspon_detected'])
        self.assertTrue(d['controller']['md32_enabled'])
        self.assertTrue(d['controller']['tx_disabled'])
        self.assertTrue(d['los'])
        self.assertFalse(d['activation_supported'])
        self.assertIsNone(d['service_ready'])
        for invalid in ('{', json.dumps(dict(status, schema_version=99))):
            sample.write_text(invalid)
            _, d = self.call()
            self.assertFalse(d['controller']['available'])
            self.assertIsNone(d['controller']['xgspon_detected'])
            self.assertIsNone(d['los'])
        sample.write_text(json.dumps(dict(status, los='true', md32_enabled='true')))
        _, d = self.call()
        self.assertIsNone(d['los'])
        self.assertIsNone(d['controller']['md32_enabled'])
        sample.write_text(json.dumps(status))
        # Ambiguous devices cannot be selected for commands or status.
        self.write(base.replace('0-0051', '1-0051') + 'status', json.dumps(status))
        self.write(base.replace('0-0051', '1-0051') + 'operation', '')
        self.assertFalse(self.call()[1]['controller']['available'])

    def test_omci_status_isolated_typed_and_does_not_prove_service(self):
        self.write('omci', '#!/bin/sh\n[ "$*" = "-i pon status" ] || exit 2\ncat "' +
                   str(self.root / 'omci.json') + '"\nexit "${OMCI_FAIL:-0}"\n').chmod(0o755)
        status = {'schema_version': 1, 'device_id': 7, 'ifindex': 9, 'state': 5,
                  'authenticated': 1, 'service_rules': 3, 'service_error': -22,
                  'rx_packets': '18446744073709551615', 'mib_objects': 281,
                  'telemetry_valid': 64, 'rx_power_nw': 19900}
        sample = self.write('omci.json', json.dumps(status))
        _, d = self.call()
        self.assertEqual(d['registration'], 5)
        self.assertEqual(d['omci']['service_error'], -22)
        self.assertEqual(d['omci']['rx_packets'], '18446744073709551615')
        self.assertEqual(d['omci']['rx_power_nw'], 19900)
        self.assertIsNone(d['omci']['tx_power_nw'])
        self.assertIsNone(d['omci']['tx_packets'])
        self.assertFalse(d['controller']['available'])
        self.assertIsNone(d['service_ready'])
        for bad in ('{', json.dumps(dict(status, schema_version=99)),
                    json.dumps(dict(status, schema_version='1'))):
            sample.write_text(bad)
            self.assertIsNone(self.call()[1]['omci'])
        sample.write_text(json.dumps(dict(status, state='5', authenticated=True, rx_packets=7)))
        _, d = self.call()
        self.assertIsNone(d['registration'])
        self.assertIsNone(d['omci']['authenticated'])
        self.assertIsNone(d['omci']['rx_packets'])
        sample.write_text(json.dumps(status))
        self.env['OMCI_FAIL'] = '1'
        self.assertIsNone(self.call()[1]['omci'])

    def test_mib_rpc_fixed_read_only_command_and_error(self):
        rpc = (REPO / 'package/luci-app-econet-xpon/root/usr/libexec/rpcd/econet-xpon').read_text()
        script = self.write('rpc', rpc.replace('/usr/sbin/q1000k-omci', str(self.root / 'omci')))
        self.write('omci', '#!/bin/sh\n[ "$*" = "-i pon mib" ] || exit 2\nprintf "%s" "$MIB_DATA"\nexit "${OMCI_FAIL:-0}"\n').chmod(0o755)
        self.env['MIB_DATA'] = '[{"class_id":277,"entity_id":32768}]'
        def call(*args):
            return subprocess.run(['busybox', 'ash', str(script), *args], env=self.env,
                                  capture_output=True, text=True)
        self.assertEqual(set(json.loads(call('list').stdout)), {'status', 'raw', 'mib'})
        d = json.loads(call('call', 'mib').stdout)
        self.assertTrue(d['available'])
        self.assertEqual(d['entities'][0]['class_id'], 277)
        self.env['OMCI_FAIL'] = '1'
        d = json.loads(call('call', 'mib').stdout)
        self.assertFalse(d['available'])
        self.assertIsNone(d['entities'])
        self.assertNotEqual(call('call', 'set').returncode, 0)

    def test_identity_fallback_and_override_validation(self):
        self.write('factory.json', json.dumps({'available': True, 'source': 'factory',
                                             'serial': 'TEST01234567', 'wan_mac': '00:11:22:33:44:55'}))
        _, d = self.call()
        self.assertTrue(d['identity']['valid'])
        self.assertEqual(d['identity']['serial_source'], 'factory')
        for sn, mac, expected in [('ABCD00112233', '02:11:22:33:44:55', True),
                                  ('ABCD00112233\nTEST01234567', '02:11:22:33:44:55', False),
                                  ('ABCD00112233', '01:11:22:33:44:55', False),
                                  ('ABCD00112233', '02:11:22:33:44:55\n00:11:22:33:44:55', False),
                                  ('ABCD00112233', '00:00:00:00:00:00', False),
                                  ('AB C00112233', '', False)]:
            self.env.update(TEST_SN=sn, TEST_MAC=mac)
            r, d = self.call('validate')
            self.assertEqual(d['valid'], expected)
            self.assertEqual(r.returncode == 0, expected)

    def test_firmware_integrity_and_private_calibration_staging(self):
        def prepare():
            return subprocess.run(['busybox', 'ash', str(self.cli), 'prepare'],
                                  capture_output=True, text=True, env=self.env)
        self.assertNotEqual(prepare().returncode, 0)
        self.assertEqual(list((self.root / 'stage').iterdir()), [])
        self.write('factory.json', json.dumps({'available': True, 'source': 'factory',
                                             'serial': 'TEST01234567', 'wan_mac': '00:11:22:33:44:55'}))
        for suffix, (data, _) in self.firmware.items():
            self.write('lib/firmware/airoha/q1000k/A60993.elf.' + suffix, data.decode())
        cal = bytes(i % 251 for i in range(513))
        (self.root / 'calibration.bin').write_bytes(cal)
        _, d = self.call()
        self.assertTrue(d['firmware']['program_verified'])
        self.assertTrue(d['firmware']['data_verified'])
        r = prepare()
        self.assertEqual(r.returncode, 0, r.stderr)
        directory = Path(r.stdout.strip())
        self.assertEqual(directory.parent, self.root / 'stage')
        self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
        staged = directory / 'xgspon-calibration.bin'
        self.assertEqual(staged.read_bytes(), cal)
        self.assertEqual(staged.stat().st_mode & 0o777, 0o600)
        staged.unlink()
        directory.rmdir()
        self.env['CAL_FAIL'] = '1'
        self.assertNotEqual(prepare().returncode, 0)
        self.assertEqual(list((self.root / 'stage').iterdir()), [])
        self.env.pop('CAL_FAIL')
        firmware = self.root / 'lib/firmware/airoha/q1000k/A60993.elf.pm'
        firmware.write_bytes(b'corrupt')
        self.assertFalse(self.call()[1]['firmware']['program_verified'])
        self.assertNotEqual(prepare().returncode, 0)
        firmware.unlink()
        firmware.symlink_to(self.root / 'calibration.bin')
        self.assertFalse(self.call()[1]['firmware']['program_verified'])
        r = subprocess.run(['busybox', 'ash', str(self.cli), 'start'],
                           capture_output=True, text=True, env=self.env)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(list((self.root / 'stage').iterdir()), [])


class FirmwareExtractionTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('extract_pon', REPO / 'scripts/q1000k-extract-pon-firmware.py')
        self.extractor = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.extractor)
        self.tmp = tempfile.TemporaryDirectory(prefix='q1000k-firmware-test-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_wrong_oem_firmware_publishes_nothing(self):
        result = subprocess.CompletedProcess([], 0, stdout=b'not OEM firmware')
        with mock.patch.object(self.extractor.subprocess, 'run', return_value=result):
            with self.assertRaises(ValueError):
                self.extractor.extract(self.root / 'oem.squashfs', self.root / 'overlay')
        self.assertEqual(list(self.root.iterdir()), [])

    def test_complete_overlay_and_failed_second_blob(self):
        blobs = {'A60993.elf.pm': b'program fixture', 'A60993.elf.dm': b'data fixture'}
        fixtures = {k: (len(v), hashlib.sha256(v).hexdigest()) for k, v in blobs.items()}
        def run(args, **kwargs):
            self.assertEqual(args[:2], ['unsquashfs', '-cat'])
            return subprocess.CompletedProcess(args, 0, stdout=blobs[Path(args[-1]).name])
        # Replace only the known firmware values, keeping extraction, hashing,
        # size validation, filesystem operations and publication code intact.
        with mock.patch.object(self.extractor, 'FIRMWARE', fixtures), \
                mock.patch.object(self.extractor.subprocess, 'run', side_effect=run):
            output = self.extractor.extract(self.root / 'oem.squashfs', self.root / 'overlay')
            for name, data in blobs.items():
                self.assertEqual((output / 'lib/firmware/airoha/q1000k' / name).read_bytes(), data)
            with self.assertRaises(ValueError):
                self.extractor.extract(self.root / 'oem.squashfs', output)
            blobs['A60993.elf.dm'] = b'corrupt'
            with self.assertRaises(ValueError):
                self.extractor.extract(self.root / 'oem.squashfs', self.root / 'incomplete')
            self.assertFalse((self.root / 'incomplete').exists())
            self.assertEqual(list(self.root.iterdir()), [output])


if __name__ == '__main__':
    unittest.main()
