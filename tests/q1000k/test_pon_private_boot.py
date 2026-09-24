#!/usr/bin/env python3
"""Verify private overlay inputs and actual first-boot UCI ordering in isolation."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import unittest
from unittest.mock import patch

import test_pon_wan
import test_pon_bench_runner

ROOT = test_pon_wan.ROOT
spec = importlib.util.spec_from_file_location('private_autostart', ROOT / 'scripts/q1000k/private-autostart.py')
PRIVATE = importlib.util.module_from_spec(spec)
spec.loader.exec_module(PRIVATE)


class PrivateBootTests(unittest.TestCase):
    setUpClass = classmethod(test_pon_wan.WanTests.setUpClass.__func__)
    setUp = test_pon_wan.WanTests.setUp
    uci_cmd = test_pon_wan.WanTests.uci_cmd
    install = test_pon_wan.WanTests.install

    def overlay(self):
        archive = self.root / 'inputs.tar'
        checks = {}
        with tarfile.open(archive, 'w') as output:
            for name in (*PRIVATE.inputs.INPUTS, 'sha256sums'):
                data = ('synthetic fixture ' + name).encode()
                entry = tarfile.TarInfo(name); entry.size = len(data)
                output.addfile(entry, io.BytesIO(data))
                if name != 'sha256sums':
                    checks[name] = (len(data), hashlib.sha256(data).hexdigest())
        self.identity = dict(serial='TEST01234567', wan_mac='02:11:22:33:44:55',
                             registration_id='', equipment_id="Q'\"$()\\", fix_vlans='1',
                             hardware_version='BGW320-500_2.1', software_version_a='BGW320_6.35.8')
        identity = self.root / 'identity.json'
        identity.write_text(json.dumps(self.identity))
        overlay = self.root / 'overlay'
        with patch.dict(PRIVATE.inputs.INPUTS, checks, clear=True):
            manifest = PRIVATE.generate(archive, identity, overlay)
        return overlay, manifest, archive, identity

    def run_defaults(self, path, code=0):
        source = path.read_text().replace('/sys/firmware/devicetree/base', str(self.root / 'dt'))
        source = source.replace('/etc/q1000k-private-autostart', str(self.root / 'private-marker'))
        run = subprocess.run(['/bin/sh', '-c', source], env=self.env, capture_output=True, text=True)
        self.assertEqual(run.returncode, code, run.stderr)
        return run

    def test_first_boot_after_bench_defaults_enables_lan_and_owned_wan(self):
        overlay, manifest, _, _ = self.overlay()
        shutil.copy2(overlay / 'etc/config/q1000k-xgspon', self.root / 'config/q1000k-xgspon')
        # Real libuci round-trip, including quote and metacharacter literals.
        for key, value in self.identity.items():
            self.assertEqual(self.uci_cmd('get', 'q1000k-xgspon.identity.' + key), value or '00' * 36)
        self.assertNotIn(self.identity['serial'], json.dumps(manifest))
        self.assertEqual(manifest['registration_source'], 'zero-default')
        for name, expected in manifest['files'].items():
            path = overlay / name
            self.assertEqual(path.stat().st_mode & 0o777, expected['mode'])
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), expected['sha256'])
        (self.root / 'config/dhcp').write_text("config dhcp 'lan'\n option interface 'lan'\n")
        (self.root / 'config/system').write_text("config system\n option hostname 'OpenWrt'\n")
        (self.root / 'dt').mkdir()
        for name in ('quantum,xgspon-bench', 'quantum,xgspon-activation-bench'):
            (self.root / 'dt' / name).touch()
        (self.root / 'private-marker').touch()
        self.install()  # 90-q1000k-xgspon-wan
        self.run_defaults(ROOT / 'package/network/utils/q1000k-xgspon-bench/files/defaults')
        self.assertEqual(self.uci_cmd('get', 'q1000k-xgspon.service.enabled'), '0')
        self.run_defaults(overlay / 'etc/uci-defaults/zz-q1000k-private-autostart')
        expected = {
            'q1000k-xgspon.service.enabled': '1', 'q1000k-xgspon.service.lower': 'ponraw',
            'network.lan.ipaddr': '192.168.0.1', 'dhcp.lan.ignore': '0',
            'dhcp.lan.ra': 'server', 'dhcp.lan.dhcpv6': 'server',
            'network.wan.auto': '0', 'network.wan6.auto': '0',
            'network.wan.ipv6': '1',
            'network.wan6.reqaddress': 'none', 'network.wan6.reqprefix': 'auto',
            'network.wan6.ip6hint': 'f', 'network.lan.ip6hint': '0',
            'network.lan.ip6class': 'wan6', 'network.lan.ip6assign': '64',
            'firewall.@zone[1].masq': '1', 'firewall.@forwarding[0].src': 'lan',
            'firewall.@defaults[0].flow_offloading': '1',
            'firewall.@defaults[0].flow_offloading_hw': '1',
            'firewall.@forwarding[0].dest': 'wan', 'network.br_lan.ports': 'lan1 lan2'}
        for key, value in expected.items():
            self.assertEqual(self.uci_cmd('get', key), value, key)
        self.assertIn('wan6', self.uci_cmd('get', 'firewall.@zone[1].network'))
        self.assertEqual(self.uci_cmd('changes'), '')
        self.assertEqual(self.uci_cmd('get', 'q1000k-xgspon.identity.equipment_id'), self.identity['equipment_id'])

    def test_bad_inputs_are_rejected_before_creating_private_overlay(self):
        _, _, archive, identity = self.overlay()
        # The production hashes reject our synthetic fixture; there is no
        # extraction fallback, substituted calibration or partial overlay.
        with self.assertRaisesRegex(ValueError, 'Wrong firmware/calibration'):
            PRIVATE.generate(archive, identity, self.root / 'bad-overlay')
        self.assertFalse((self.root / 'bad-overlay').exists())

    def test_private_build_failure_cleans_owned_overlay_and_restores_configs(self):
        overlay, _, _, _ = self.overlay()
        selected = self.root / 'selection'
        (selected / 'openwrt/files').mkdir(parents=True)
        (selected / 'openwrt/files/build_info').write_text('bench')
        (selected / 'openwrt/.config').write_text('bench-config')
        (self.root / '.config').write_text('normal-config')
        (self.root / 'backup').mkdir()
        with self.assertRaisesRegex(RuntimeError, 'interrupted'):
            with test_pon_bench_runner.BUILD.bench_config(self.root, selected, self.root / 'backup', overlay):
                self.assertEqual((self.root / 'files/etc/config/q1000k-xgspon').stat().st_mode & 0o777, 0o600)
                raise RuntimeError('interrupted')
        self.assertEqual((self.root / '.config').read_text(), 'normal-config')
        self.assertFalse((self.root / 'files').exists())


if __name__ == '__main__':
    unittest.main()
