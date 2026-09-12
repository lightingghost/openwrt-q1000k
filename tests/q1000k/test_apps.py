#!/usr/bin/env python3
"""Exercise the actual RPC scripts in a fake sysfs/procfs tree using BusyBox ash.

Requires a built OpenWrt host jshn and host jsonfilter on PATH. Never accesses
router hardware. Run from any directory: python3 tests/q1000k/test_apps.py
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class AppTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='q1000k-rpc-')
        self.root = Path(self.tmp.name)
        self.env = dict(os.environ, PATH=f'{REPO}/staging_dir/host/bin:{os.environ["PATH"]}')
        self.scripts = {}
        for app in ('npu', 'flowsense'):
            source = REPO / f'package/luci-app-airoha-{app}/root/usr/libexec/rpcd/luci.airoha_{app}'
            script = source.read_text().replace('/usr/share/libubox/jshn.sh', str(REPO / 'staging_dir/host/share/libubox/jshn.sh'))
            # Redirect only absolute runtime paths, preserving regexes and URLs.
            script = re.sub(r'(?<![a-zA-Z0-9])/(sys|proc|etc|lib/firmware|tmp)/', lambda m: f'{self.root}/{m[1]}/', script)
            script = script.replace('dmesg |', 'true |')
            target = self.root / f'{app}.sh'
            target.write_text(script)
            self.scripts[app] = target

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, path, text):
        target = self.root / path.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
        return target

    def rpc(self, app, method, args=None):
        result = subprocess.run(['busybox', 'ash', str(self.scripts[app]), 'call', method],
                                input=json.dumps(args or {}) + '\n', capture_output=True,
                                text=True, env=self.env, check=True)
        return json.loads(result.stdout)

    def test_temperatures(self):
        self.write('/sys/class/thermal/thermal_zone0/type', 'CPU "package"\n')
        self.write('/sys/class/thermal/thermal_zone0/temp', '53250\n')
        self.write('/sys/class/thermal/thermal_zone1/temp', 'not ready\n')
        self.write('/sys/class/hwmon/hwmon0/name', 'phy\n')
        self.write('/sys/class/hwmon/hwmon0/temp1_input', '-2500\n')
        self.write('/sys/class/hwmon/hwmon0/temp1_label', 'Board\n')
        sensors = self.rpc('npu', 'getTemperatures')['sensors']
        self.assertEqual([(x['name'], x['millidegrees']) for x in sensors],
                         [('CPU "package"', 53250), ('phy Board', -2500)])

    def test_absent_hardware(self):
        self.assertEqual(self.rpc('npu', 'getTemperatures'), {'sensors': []})
        self.assertEqual(self.rpc('flowsense', 'getWifiStats'), {'available': False, 'bands': []})
        for app in self.scripts:
            status = self.rpc(app, 'getStatus')
            self.assertFalse(status['npu_loaded'])
            self.assertEqual(status['offload_bound'], 0)
            self.assertEqual(status['npu_cores'], 0)
            self.assertFalse(self.rpc(app, 'getFrameEngine')['available'])

    def test_firmware_default_and_override(self):
        self.write('/lib/firmware/airoha/en7581_npu_rv32.bin', '1.0.0-TLB2.3-test\n')
        self.write('/sys/bus/platform/drivers/airoha-npu/1e900000.npu/uevent', '')
        for app in self.scripts:
            self.assertTrue(self.rpc(app, 'getStatus')['npu_loaded'])
            self.assertIn('TLB2.3', self.rpc(app, 'getStatus')['npu_version'])
        self.write('/proc/device-tree/soc/npu@1e900000/firmware-name', 'airoha/custom_rv32.bin\0airoha/custom_data.bin\0')
        self.write('/lib/firmware/airoha/custom_rv32.bin', '1.0.0-TLB9.8-test\n')
        for app in self.scripts:
            self.assertIn('TLB9.8', self.rpc(app, 'getStatus')['npu_version'])

    def test_bridge_filters_persist_and_validate(self):
        for app in self.scripts:
            for tag, method, num in [('vlan', 'setVlanOffload', 14),
                                     ('pppoe', 'setPPPoEOffload' if app == 'npu' else 'setPppoeOffload', 15)]:
                path = self.write(f'/proc/sys/net/bridge/bridge-nf-filter-{tag}-tagged', '0\n')
                for enabled in (1, 0):
                    self.assertEqual(self.rpc(app, method, {'enabled': enabled})['enabled'], enabled)
                    self.assertEqual(path.read_text().strip(), str(enabled))
                    self.assertEqual((self.root / f'etc/sysctl.d/{num}-{tag}-offload.conf').read_text(),
                                     f'net.bridge.bridge-nf-filter-{tag}-tagged={enabled}\n')
                for args in ({}, {'enabled': 2}, {'enabled': '.*'}, {'enabled': '1\n0'}):
                    self.assertIn('error', self.rpc(app, method, args))
                    self.assertEqual(path.read_text(), '0\n')
                path.unlink()
                self.assertIn('error', self.rpc(app, method, {'enabled': 1}))

    def test_governor_and_frequency_membership(self):
        base = '/sys/devices/system/cpu/cpufreq/policy0/'
        self.write(base+'scaling_available_governors', 'performance schedutil\n')
        target = self.write(base+'scaling_governor', 'schedutil\n')
        self.assertIn('error', self.rpc('npu', 'setGovernor', {'governor': '.*'}))
        self.assertEqual(target.read_text(), 'schedutil\n')
        self.assertEqual(self.rpc('npu', 'setGovernor', {'governor':'performance'})['result'], 'ok')
        self.write(base+'scaling_available_frequencies', '500000 1200000\n')
        freq = self.write(base+'scaling_max_freq', '1200000\n')
        self.assertIn('error', self.rpc('npu', 'setMaxFreq', {'freq': 1400000}))
        self.assertEqual(freq.read_text(), '1200000\n')
        self.assertIn('error', self.rpc('npu', 'setOverclock', {'freq_mhz': 1400}))

    def test_ppe_hex_indices_and_counts(self):
        bind = 'a000 BND IPv4 5T orig=192.0.2.1:1->192.0.2.2:2 eth=aa:bb:cc:dd:ee:ff->11:22:33:44:55:66\n'
        self.write('/sys/kernel/debug/ppe/bind', bind)
        self.write('/sys/kernel/debug/ppe/entries', bind + 'ffff UNB IPv4 5T orig=192.0.2.3:1->192.0.2.4:2\n')
        ppe = self.rpc('flowsense', 'getPpeEntries')
        self.assertEqual(ppe['bnd']['total'], 1)
        self.assertEqual(ppe['unb']['total'], 1)
        self.assertEqual(self.rpc('flowsense', 'getNpuBypass')['offload_bound'], 1)

    def test_jitter_without_route_and_with_gateway(self):
        source = REPO / 'package/luci-app-airoha-flowsense/root/usr/libexec/npu-jitter-daemon'
        script = source.read_text().replace('while true; do', 'for iteration in 1; do').replace('sleep "$INTERVAL"', ':')
        script = script.replace('/tmp/npu-jitter.json', str(self.root / 'jitter.json'))
        # BusyBox standalone ash can prefer its own ip/ping applets over PATH.
        script = script.replace('ip -4 route', 'fixture_ip -4 route').replace('ping -c', 'fixture_ping -c')
        target = self.root / 'jitter.sh'
        target.write_text(script)
        commands = self.root / 'commands'
        commands.mkdir()
        ip = commands / 'fixture_ip'
        ip.write_text('#!/bin/sh\nexit 0\n')
        ip.chmod(0o755)
        ping = commands / 'fixture_ping'
        ping.write_text('#!/bin/sh\necho called >> "' + str(self.root / 'ping-calls') + '"\necho "round-trip min/avg/max = 2.0/2.0/2.0 ms"\n')
        ping.chmod(0o755)
        env = dict(self.env, PATH=str(commands) + ':' + self.env['PATH'])
        def run(*args):
            return subprocess.run(['busybox', 'ash', str(target), *args], env=env, capture_output=True, text=True)
        self.assertEqual(run().returncode, 0)
        self.assertFalse(json.loads((self.root / 'jitter.json').read_text())['available'])
        self.assertFalse((self.root / 'ping-calls').exists())
        ip.write_text('#!/bin/sh\necho "default via 192.0.2.1 dev br-lan"\n')
        self.assertEqual(run().returncode, 0)
        result = json.loads((self.root / 'jitter.json').read_text())
        self.assertEqual(result['target'], '192.0.2.1')
        self.assertTrue(result['reachable'])
        self.assertEqual(result['last_ping'], 2)
        self.assertNotEqual(run("host'; exit 0").returncode, 0)


if __name__ == '__main__':
    unittest.main()
