#!/usr/bin/env python3
"""Run the bench lifecycle with private files and inert module/network models."""
import json
import re
import subprocess
import sys
import unittest
from pathlib import Path
import test_xgspon
import test_pon_wan

ROOT = test_xgspon.REPO
PACKAGE = ROOT / 'package/network/utils/q1000k-xgspon-bench'
MODULES = ('q1000k_pon_control airoha_ecnt_hook airoha_ecnt_scu airoha_ecnt_pon_phy '
           'airoha_ecnt_xpon phy_10g xpon omci xpon_10g').split()


class BenchTests(unittest.TestCase):
    def setUp(self):
        backend = test_xgspon.BackendTests()
        backend.setUp()
        self.addCleanup(backend.tearDown)
        self.root, self.env, self.write = backend.root, backend.env, backend.write
        self.env['BENCH_TEST_ROOT'] = str(self.root)
        self.dt = 'sys/firmware/devicetree/base/'
        self.write(self.dt + 'quantum,xgspon-bench', '')
        self.write(self.dt + 'soc/spi@1fa10000/status', 'disabled\0')
        self.write('tmp/sysinfo/board_name', 'quantum,q1000k-ubi\n')
        self.write('proc/mounts', 'rootfs / rootfs rw 0 0\n')
        self.write('sys/class/net/ponraw/flags', '0x1002\n')
        (self.root / 'var/run').mkdir(parents=True)
        self.calibration = self.write('tmp/calibration', 'c' * 513)
        self.controller = self.root / 'sys/bus/i2c/drivers/q1000k-pon-control/0-0051'
        self.controller.mkdir(parents=True)
        (self.controller / 'status').touch()
        (self.controller / 'operation').touch()
        for suffix, (data, _) in backend.firmware.items():
            self.write('lib/firmware/airoha/q1000k/A60993.elf.' + suffix, data.decode())
        self.write('uci', '''#!/bin/sh
case "$*" in
*network.lan.ipaddr) echo "${BENCH_IP:-192.168.0.1}" ;;
*dhcp.lan.ignore) echo 1 ;;
*service.enabled) echo 0 ;;
esac
''').chmod(0o755)
        fake = '''import json,os,pathlib,sys
root=pathlib.Path(os.environ['BENCH_TEST_ROOT'])
action=pathlib.Path(sys.argv[0]).name
args=sys.argv[1:]
ctl=root/'sys/bus/i2c/drivers/q1000k-pon-control/0-0051'
if action=='cat':
    if args==[str(ctl/'status')]:
        initialized=(ctl/'operation').read_text().strip()=='initialize'
        data=dict(schema_version=1, mode='xgspon' if initialized else 'off',
                  tx_inhibited=True, tx_disabled=True, last_error=0,
                  md32_enabled=initialized, firmware_verified=initialized,
                  calibration_supplied=initialized, los=True)
        data.update(json.loads(os.environ.get('BENCH_STATUS','{}')))
        print(json.dumps(data))
    else:
        for name in args:
            sys.stdout.buffer.write(pathlib.Path(name).read_bytes())
    sys.exit(0)
if action=='sleep': sys.exit(0)
with (root/'calls').open('a') as output: output.write(json.dumps([action]+args)+'\\n')
if os.environ.get('BENCH_FAIL')==action+':'+args[0]: sys.exit(1)
if action in ('modprobe','insmod'):
    if args[0]=='xpon_10g':
        # Model ubox: only insmod forwards command-line parameters.
        params=dict(arg.split('=',1) for arg in args[1:]) if action=='insmod' else {}
        if not {'wan_mac','pon_serial','pon_reg_id','pon_lower'} <= params.keys(): sys.exit(1)
    (root/'sys/module'/args[0]).mkdir(parents=True)
    if args[0]=='xpon_10g':
        (root/'proc/xgpon').mkdir(parents=True)
        (root/'proc/xgpon/status').write_text('protocol_error=0\\n')
elif action=='rmmod':
    if args[0]=='q1000k_pon_control': assert (ctl/'operation').read_text()=='off\\n'
    (root/'sys/module'/args[0]).rmdir()
elif action=='ip': assert args[:4]==['link','set','dev','ponraw']
elif action=='omci':
    assert args==['-i','pon','status']
    print('{"schema_version":1,"service_error":0}')
else: raise AssertionError(action)
'''
        for name in ('modprobe', 'insmod', 'rmmod', 'ip', 'cat', 'sleep', 'omci'):
            self.write(name, '#!' + sys.executable + '\n' + fake).chmod(0o755)
        source = (PACKAGE / 'files/bench').read_text()
        # All target paths and every hardware-changing executable are replaced
        # explicitly. A fixture PATH mistake cannot load a real module.
        source = re.sub(r'(?<![A-Za-z0-9])/(sys|proc|tmp|var/run)/',
                        lambda m: str(self.root) + '/' + m[1] + '/', source)
        source = source.replace('/lib/q1000k-xgspon/common.sh', str(backend.common))
        source = source.replace('/usr/sbin/q1000k-omci', str(self.root / 'omci'))
        for name in ('modprobe', 'insmod', 'rmmod', 'ip', 'cat', 'sleep'):
            source = re.sub(r'(?<![A-Za-z0-9_/-])' + name + r'(?= )',
                            '"' + str(self.root / name) + '"', source)
        self.script = self.write('bench', source)

    def run_bench(self, mode='stack', success=True, acknowledged=True):
        args = [mode]
        if mode != 'status':
            args += [str(self.calibration)]
            if acknowledged:
                args += ['--fiber-disconnected']
        result = subprocess.run(['busybox', 'ash', str(self.script), *args],
                                env=self.env, text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def calls(self):
        path = self.root / 'calls'
        return [json.loads(x) for x in path.read_text().splitlines()] if path.exists() else []

    def test_read_only_status_and_missing_acknowledgement(self):
        self.run_bench('status')
        self.run_bench(success=False, acknowledged=False)
        self.assertEqual(self.calls(), [])

    def test_persistent_storage_and_wrong_subnet_block_all_mutations(self):
        for name, content in [('sys/class/mtd/mtd0', ''),
                              ('sys/class/ubi/ubi0', ''),
                              (self.dt + 'chosen/rootdisk', 'bad')]:
            with self.subTest(name=name):
                path = self.write(name, content)
                self.run_bench(success=False)
                path.unlink()
        self.env['BENCH_IP'] = '192.168.1.1'
        self.run_bench(success=False)
        self.assertEqual(self.calls(), [])

    def test_controller_cycle_and_cleanup(self):
        self.run_bench('controller')
        self.assertEqual(self.calls(), [['modprobe', MODULES[0]], ['rmmod', MODULES[0]]])
        self.assertEqual((self.controller / 'calibration').read_bytes(), self.calibration.read_bytes())
        self.assertFalse((self.root / 'var/run/q1000k-pon-bench.lock').exists())

    def test_full_stack_cycle_and_reverse_cleanup(self):
        self.run_bench()
        calls = self.calls()
        loads = [c for c in calls if c[0] in ('modprobe', 'insmod')]
        self.assertEqual([c[1] for c in loads], MODULES)
        self.assertEqual(loads[-1], ['insmod', 'xpon_10g', 'wan_mac=02:00:00:00:00:01',
                                   'pon_serial=TEST00000001', 'pon_reg_id=' + '0' * 72,
                                   'pon_lower=ponraw'])
        self.assertEqual([c[1] for c in calls if c[0] == 'rmmod'], MODULES[::-1])
        self.assertEqual(calls[-1], ['ip', 'link', 'set', 'dev', 'ponraw', 'down'])
        self.assertEqual(len([c for c in calls if c[0] == 'omci']), 5)

    def test_unproven_tx_inhibit_and_los_fail_closed(self):
        for value in ({'tx_inhibited': False}, {'tx_inhibited': 'true'},
                      {'tx_disabled': False}, {'los': False}, {'schema_version': 99}):
            with self.subTest(value=value):
                self.env['BENCH_STATUS'] = json.dumps(value)
                (self.controller / 'operation').write_text('')
                self.run_bench(success=False)
                self.assertFalse((self.root / 'sys/module/phy_10g').exists())
                self.assertEqual((self.controller / 'operation').read_text(), 'off\n')

    def test_partial_load_failure_and_unload_failure_preserve_dependencies(self):
        self.env['BENCH_FAIL'] = 'modprobe:phy_10g'
        self.run_bench(success=False)
        self.assertEqual([c[1] for c in self.calls() if c[0] == 'rmmod'], MODULES[:5][::-1])
        (self.root / 'calls').unlink()
        self.env['BENCH_FAIL'] = 'rmmod:xpon_10g'
        self.run_bench(success=False)
        self.assertEqual([c for c in self.calls() if c[0] == 'rmmod'], [['rmmod', 'xpon_10g']])
        self.assertTrue((self.root / 'sys/module/q1000k_pon_control').exists())
        self.assertNotIn(['ip', 'link', 'set', 'dev', 'ponraw', 'down'], self.calls())


class BenchNetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Reuse the isolated real-UCI build without inheriting the WAN tests.
        test_pon_wan.WanTests.setUpClass()
        cls.addClassCleanup(test_pon_wan.WanTests.doClassCleanups)

    def test_defaults_preserve_copper_ports_and_disable_dhcp(self):
        fixture = test_pon_wan.WanTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        for name, content in [('dhcp', "config dhcp 'lan'\n option interface 'lan'\n option ra 'server'\n"),
                              ('system', 'config system\n option hostname OpenWrt\n'),
                              ('q1000k-xgspon', "config service 'service'\n option enabled '1'\n")]:
            (fixture.root / 'config' / name).write_text(content)
        marker = fixture.root / 'marker'
        marker.touch()
        source = (PACKAGE / 'files/defaults').read_text().replace(
            '/sys/firmware/devicetree/base/quantum,xgspon-bench', str(marker))
        script = fixture.root / 'defaults'
        script.write_text(source)
        subprocess.run(['/bin/sh', str(script)], env=fixture.env, check=True, capture_output=True)
        for key, value in [('network.lan.ipaddr', '192.168.0.1'), ('dhcp.lan.ignore', '1'),
                           ('dhcp.lan.ra', 'disabled'), ('dhcp.lan.dhcpv6', 'disabled'),
                           ('network.br_lan.ports', 'lan1 lan2'), ('q1000k-xgspon.service.enabled', '0')]:
            self.assertEqual(fixture.uci_cmd('get', key), value)


if __name__ == '__main__':
    unittest.main()
