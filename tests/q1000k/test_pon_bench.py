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
        for module in MODULES:
            filename = 'q1000k-pon-control' if module == 'q1000k_pon_control' else module
            self.write('lib/modules/q1000k-fixture/' + filename + '.ko', 'inert fixture module\n')
        self.dt = 'sys/firmware/devicetree/base/'
        self.write(self.dt + 'quantum,xgspon-bench', '')
        self.write(self.dt + 'soc/spi@1fa10000/status', 'disabled\0')
        self.write('tmp/sysinfo/board_name', 'quantum,q1000k-ubi\n')
        self.write('proc/mounts', 'rootfs / rootfs rw 0 0\n')
        self.write('proc/sys/kernel/panic', '0\n')
        self.write('sys/class/leds/green:wan-1/brightness', '0\n')
        self.write('sys/class/leds/red:wan/brightness', '1\n')
        self.write('sys/class/net/ponraw/flags', '0x1002\n')
        (self.root / 'var/run').mkdir(parents=True)
        self.calibration = self.write('tmp/calibration', 'c' * 513)
        self.controller = self.root / 'sys/bus/i2c/drivers/q1000k-pon-control/0-0051'
        self.controller.mkdir(parents=True)
        (self.controller / 'status').touch()
        self.write(str(self.controller.relative_to(self.root)) + '/receiver_status',
                   '{"schema_version":1,"receiver_status":true}\n')
        (self.controller / 'operation').touch()
        for suffix, (data, _) in backend.firmware.items():
            self.write('lib/firmware/airoha/q1000k/A60993.elf.' + suffix, data.decode())
        self.write('uci', '''#!/bin/sh
case "$*" in
*network.lan.ipaddr) echo "${BENCH_IP:-192.168.255.1}" ;;
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
                  calibration_supplied=initialized, los=True,
                  bench_md32_a0=bool(int(os.environ.get('BENCH_OEM_MD32','0'))),
                  bench_rx_output=int(os.environ.get('BENCH_RX_OUTPUT','0')))
        data.update(json.loads(os.environ.get('BENCH_STATUS','{}')))
        print(json.dumps(data))
    elif args==[str(root/'sys/module/xpon_10g/parameters/rx_bench_status')]:
        counter=root/'rx-count'; n=int(counter.read_text())+1 if counter.exists() else 1; counter.write_text(str(n))
        lit=os.environ.get('BENCH_FIBER')=='connected'
        params=json.loads((root/'module-params').read_text())
        retry=params.get('rx_reacquire')=='1'
        data=dict(rx_bench=True, registration_enabled=False, tx_inhibited=True,tx_enabled=False,
                  gain_restore_enabled=params.get('rx_restore_gain')=='1', receiver_version=5, rx_power_valid=True, rx_power_nw=19900,
                  pll_restore_enabled=params.get('rx_restore_pll')=='1', reacquire_enabled=retry, reacquire_attempts=1 if retry and n>=15 else 0,
                  mac_irq_mask=0, controller_los=not lit,phy_los=not lit,synced=lit,sync_status=0,
                  frames=n if lit else 0,lof=0,fec_total=n if lit else 0,fec_corrected=0,
                  fec_uncorrected=0,irq_calls=0,poll_calls=n,sampled_ms=n*1000)
        data.update(json.loads(os.environ.get('BENCH_RX_STATUS','{}')))
        print(json.dumps(data))
    elif args==[str(root/'sys/module/xpon_10g/parameters/rx_bench_diagnostics')]:
        n=int((root/'rx-count').read_text())
        params=json.loads((root/'module-params').read_text())
        mode=int(params.get('rx_probe',0))
        attempts=int(params.get('rx_reacquire')=='1' and n>=15)
        data=dict(diagnostics_version=3,probe=mode,attempts=attempts,writes=attempts if mode else 0,
                  sampled_ms=n*1000,checker_control=5,data_route_control=0,bist_lane_control=0,
                  tdc_ncpo=0,fifo_clock_status=0)
        data.update(json.loads(os.environ.get('BENCH_DIAGNOSTICS','{}')))
        print(json.dumps(data))
    else:
        for name in args:
            sys.stdout.buffer.write(pathlib.Path(name).read_bytes())
    sys.exit(0)
if action=='sleep': sys.exit(0)
if action=='uname':
    assert args==['-r']
    print('q1000k-fixture')
    sys.exit(0)
with (root/'calls').open('a') as output: output.write(json.dumps([action]+args)+'\\n')
if os.environ.get('BENCH_FAIL')==action+':'+args[0]: sys.exit(1)
if action in ('modprobe','insmod'):
    if action=='insmod':
        image=pathlib.Path(args[0])
        if (not image.is_file() or image.parent!=root/'lib/modules/q1000k-fixture' or
                image.suffix!='.ko'): sys.exit(1)
        module=image.stem.replace('-','_')
    else:
        module=args[0]
    if module=='xpon_10g':
        # Model ubox: only insmod forwards command-line parameters.
        params=dict(arg.split('=',1) for arg in args[1:]) if action=='insmod' else {}
        if not {'wan_mac','pon_serial','pon_reg_id','pon_lower'} <= params.keys(): sys.exit(1)
        (root/'module-params').write_text(json.dumps(params))
    (root/'sys/module'/module).mkdir(parents=True)
    if module=='xpon_10g':
        (root/'proc/xgpon').mkdir(parents=True)
        (root/'proc/xgpon/status').write_text('protocol_error=0\\n')
elif action=='rmmod':
    if args[0]=='q1000k_pon_control': assert (ctl/'operation').read_text()=='off\\n'
    (root/'sys/module'/args[0]).rmdir()
elif action=='ip': assert args[:4]==['link','set','dev','ponraw']
elif action=='omci':
    assert args==['-i','pon','status']
    data=dict(schema_version=1,state=1,onu_id=65535,gem_port_id=65535,agent_enabled=1,
              agent_operational=0,authenticated=0,service_rules=0,service_error=0,
              rx_packets='0',tx_packets='0',tx_errors='0')
    data.update(json.loads(os.environ.get('BENCH_OMCI_STATUS','{}')))
    print(json.dumps(data))
else: raise AssertionError(action)
'''
        for name in ('modprobe', 'insmod', 'rmmod', 'ip', 'cat', 'sleep', 'omci', 'uname'):
            self.write(name, '#!' + sys.executable + '\n' + fake).chmod(0o755)
        source = (PACKAGE / 'files/bench').read_text()
        # All target paths and every hardware-changing executable are replaced
        # explicitly. A fixture PATH mistake cannot load a real module.
        source = re.sub(r'(?<![A-Za-z0-9])/(sys|proc|tmp|var/run)/',
                        lambda m: str(self.root) + '/' + m[1] + '/', source)
        source = source.replace('/lib/q1000k-xgspon/common.sh', str(backend.common))
        source = source.replace('/lib/modules/', str(self.root/'lib/modules') + '/')
        source = source.replace('/usr/sbin/q1000k-omci', str(self.root / 'omci'))
        for name in ('modprobe', 'insmod', 'rmmod', 'ip', 'cat', 'sleep', 'uname'):
            source = re.sub(r'(?<![A-Za-z0-9_/-])' + name + r'(?= )',
                            '"' + str(self.root / name) + '"', source)
        self.script = self.write('bench', source)

    def run_bench(self, mode='stack', success=True, acknowledged=True, fiber='disconnected', reacquire=False, samples=None, restore_pll=False, restore_gain=False, probe=None, oem_md32=False, rx_output=None):
        args = [mode]
        if mode != 'status':
            args += [str(self.calibration)]
            if acknowledged:
                args += ['--fiber-' + fiber]
            if probe:
                args += ['--probe',probe]
            if oem_md32:
                args += ['--oem-md32']
            if rx_output:
                args += ['--rx-output', rx_output]
            if restore_gain:
                args += ['--restore-gain']
            if restore_pll:
                args += ['--restore-pll']
            if reacquire:
                args += ['--reacquire-once']
            if samples is not None:
                args += ['--samples', str(samples)]
        result = subprocess.run(['busybox', 'ash', str(self.script), *args],
                                env=self.env, text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def calls(self):
        path = self.root / 'calls'
        return [json.loads(x) for x in path.read_text().splitlines()] if path.exists() else []

    def module_file(self, module):
        filename = 'q1000k-pon-control' if module == 'q1000k_pon_control' else module
        return str(self.root/'lib/modules/q1000k-fixture'/(filename + '.ko'))

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
        for address in ('192.168.1.1', '192.168.0.1'):
            self.env['BENCH_IP'] = address
            self.run_bench(success=False)
        self.assertEqual(self.calls(), [])

    def test_controller_cycle_and_cleanup(self):
        self.run_bench('controller')
        self.assertEqual(self.calls(), [['insmod', self.module_file(MODULES[0]), 'bench_md32_a0=0', 'bench_rx_output=0'], ['rmmod', MODULES[0]]])
        self.assertEqual((self.controller / 'calibration').read_bytes(), self.calibration.read_bytes())
        self.assertFalse((self.root / 'var/run/q1000k-pon-bench.lock').exists())

    def test_missing_canonical_controller_file_never_claims_module_ownership(self):
        Path(self.module_file('q1000k_pon_control')).unlink()
        result = self.run_bench('controller', success=False)
        self.assertIn('Cannot load q1000k_pon_control.', result.stderr)
        self.assertEqual(self.calls(), [['insmod', self.module_file('q1000k_pon_control'),
                                        'bench_md32_a0=0', 'bench_rx_output=0']])
        self.assertFalse((self.root/'sys/module/q1000k_pon_control').exists())
        self.assertFalse((self.root/'var/run/q1000k-pon-bench.lock').exists())

    def test_receiver_read_failure_stops_before_phy_and_cleans_controller(self):
        (self.controller / 'receiver_status').unlink()
        self.run_bench(success=False)
        self.assertEqual(self.calls(), [['insmod', self.module_file(MODULES[0]), 'bench_md32_a0=0', 'bench_rx_output=0'], ['rmmod', MODULES[0]]])
        self.assertEqual((self.controller / 'operation').read_text(), 'off\n')

    def test_panic_reboot_blocks_mutations_but_allows_status(self):
        self.write('proc/sys/kernel/panic', '3\n')
        self.run_bench('status')
        self.run_bench(success=False)
        self.assertEqual(self.calls(), [])

    def test_full_stack_cycle_and_reverse_cleanup(self):
        self.run_bench()
        calls = self.calls()
        loads = [c for c in calls if c[0] in ('modprobe', 'insmod')]
        self.assertEqual([Path(c[1]).stem.replace('-', '_') if c[0] == 'insmod' else c[1] for c in loads], MODULES)
        self.assertEqual(loads[-1], ['insmod', self.module_file('xpon_10g'), 'rx_bench=0', 'rx_reacquire=0', 'rx_restore_pll=0', 'rx_restore_gain=0', 'rx_probe=0', 'wan_mac=02:00:00:00:00:01',
                                   'pon_serial=TEST00000001', 'pon_reg_id=' + '0' * 72,
                                   'pon_lower=ponraw'])
        self.assertEqual([c[1] for c in calls if c[0] == 'rmmod'], MODULES[::-1])
        self.assertEqual(calls[-1], ['ip', 'link', 'set', 'dev', 'ponraw', 'down'])
        self.assertEqual(len([c for c in calls if c[0] == 'omci']), 5)

    def test_receive_dark_cycle(self):
        self.run_bench('receive')
        loads=[c for c in self.calls() if c[0]=='insmod']
        self.assertIn('rx_bench=1', loads[-1])
        self.assertEqual([c[1] for c in self.calls() if c[0]=='rmmod'], MODULES[::-1])
        self.assertEqual((self.root/'rx-count').read_text(), '30')

    def test_long_window_keeps_one_attempt_and_unloads_every_module(self):
        self.env['BENCH_FIBER']='connected'
        self.env['BENCH_STATUS']=json.dumps({'los': False})
        result=self.run_bench('receive', fiber='connected', reacquire=True, samples=180)
        self.assertEqual((self.root/'rx-count').read_text(), '180')
        self.assertIn('bench_window mode=receive samples=180 reacquire=1', result.stdout)
        self.assertEqual([c[1] for c in self.calls() if c[0]=='rmmod'], MODULES[::-1])

    def test_probe_is_exclusive_and_forwards_immutable_mode(self):
        self.env['BENCH_FIBER']='connected'
        self.env['BENCH_STATUS']=json.dumps({'los':False})
        self.run_bench('receive',fiber='connected',probe='checker',success=False)
        self.run_bench('receive',fiber='connected',probe='checker',reacquire=True,restore_pll=True,success=False)
        self.assertEqual(self.calls(),[])
        self.run_bench('receive',fiber='connected',probe='checker',reacquire=True)
        self.assertIn('rx_probe=10',[c for c in self.calls() if c[0]=='insmod'][-1])

    def test_diagnostic_generator_guard_fails_and_cleans_up(self):
        self.env['BENCH_DIAGNOSTICS']=json.dumps({'checker_control':256})
        self.run_bench('receive',success=False)
        self.assertEqual([c[1] for c in self.calls() if c[0]=='rmmod'],MODULES[::-1])

    def test_bad_windows_fail_before_any_mutation(self):
        for samples in ('', '-1', '0', '29', '31', '181', '30 --reacquire-once'):
            self.run_bench('receive', samples=samples, success=False)
        self.run_bench('stack', samples=30, success=False)
        self.assertEqual(self.calls(), [])

    def test_receive_connected_sync_and_frame_progress(self):
        self.env['BENCH_FIBER']='connected'
        self.env['BENCH_STATUS']=json.dumps({'los': False})
        self.run_bench('receive', fiber='connected')

    def test_reacquire_requires_connected_receive_before_mutation(self):
        self.run_bench('receive', reacquire=True, success=False)
        self.run_bench('stack', fiber='connected', reacquire=True, success=False)
        self.assertEqual(self.calls(), [])

    def test_new_controller_and_phy_selection_reaches_modules(self):
        self.env.update(BENCH_FIBER='connected', BENCH_OEM_MD32='1', BENCH_RX_OUTPUT='3')
        self.env['BENCH_STATUS'] = json.dumps({'los': False})
        self.run_bench('receive', fiber='connected', reacquire=True,
                       probe='combined-auto', oem_md32=True, rx_output='600-boost')
        loads = [c for c in self.calls() if c[0] == 'insmod']
        self.assertEqual(loads[0], ['insmod', self.module_file('q1000k_pon_control'),
                                   'bench_md32_a0=1', 'bench_rx_output=3'])
        self.assertIn('rx_probe=21', loads[-1])
        self.assertEqual([c[1] for c in self.calls() if c[0]=='rmmod'], MODULES[::-1])

    def test_reacquire_passes_explicit_parameter_and_collects_attempt(self):
        self.env['BENCH_FIBER']='connected'
        self.env['BENCH_STATUS']=json.dumps({'los': False})
        result=self.run_bench('receive', fiber='connected', reacquire=True)
        self.assertIn('rx_reacquire=1', [c for c in self.calls() if c[0]=='insmod'][-1])
        observations=[json.loads(x) for x in result.stdout.splitlines() if x.startswith('{')]
        rx=[x for x in observations if x.get('rx_bench')]
        self.assertEqual([rx[0]['reacquire_attempts'],rx[-1]['reacquire_attempts']],[0,1])

    def test_pll_restoration_requires_recovery_before_mutation(self):
        self.run_bench('receive', fiber='connected', restore_pll=True, success=False)
        self.assertEqual(self.calls(), [])

    def test_pll_restoration_is_forwarded_and_observed(self):
        self.env['BENCH_FIBER']='connected'
        self.env['BENCH_STATUS']=json.dumps({'los': False})
        result=self.run_bench('receive', fiber='connected', reacquire=True, restore_pll=True)
        self.assertIn('rx_restore_pll=1', [c for c in self.calls() if c[0]=='insmod'][-1])
        rx=[json.loads(x) for x in result.stdout.splitlines() if x.startswith('{') and '"rx_bench"' in x]
        self.assertTrue(all(x['pll_restore_enabled'] for x in rx))

    def test_gain_restoration_is_guarded_forwarded_and_observed(self):
        self.env['BENCH_FIBER']='connected'
        self.env['BENCH_STATUS']=json.dumps({'los': False})
        self.run_bench('receive', fiber='connected', restore_gain=True, success=False)
        self.run_bench('receive', fiber='connected', reacquire=True, restore_gain=True)
        self.assertIn('rx_restore_gain=1', [c for c in self.calls() if c[0]=='insmod'][-1])
        self.assertEqual([c[1] for c in self.calls() if c[0]=='rmmod'], MODULES[::-1])

    def test_reacquire_rejects_repeated_attempts_and_cleans_up(self):
        self.env['BENCH_FIBER']='connected'
        self.env['BENCH_STATUS']=json.dumps({'los': False})
        self.env['BENCH_RX_STATUS']=json.dumps({'reacquire_attempts': 2})
        self.run_bench('receive', fiber='connected', reacquire=True, success=False)
        self.assertEqual([c[1] for c in self.calls() if c[0]=='rmmod'], MODULES[::-1])

    def test_receive_guards_reject_tx_and_cleanup(self):
        self.env['BENCH_RX_STATUS']=json.dumps({'tx_enabled': True})
        self.run_bench('receive', success=False)
        self.assertEqual([c[1] for c in self.calls() if c[0]=='rmmod'], MODULES[::-1])

    def test_receive_rejects_assigned_onu(self):
        self.env['BENCH_OMCI_STATUS']=json.dumps({'onu_id': 17})
        self.run_bench('receive', success=False)
        self.assertEqual([c[1] for c in self.calls() if c[0]=='rmmod'], MODULES[::-1])

    def test_connected_without_frames_fails(self):
        self.env['BENCH_FIBER']='connected'
        self.env['BENCH_STATUS']=json.dumps({'los': False})
        self.env['BENCH_RX_STATUS']=json.dumps({'frames': 0})
        self.run_bench('receive', fiber='connected', success=False)

    def test_connected_never_runs_normal_stack(self):
        self.run_bench('stack', fiber='connected', success=False)
        self.assertEqual(self.calls(), [])

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
        for key, value in [('network.lan.ipaddr', '192.168.255.1'), ('dhcp.lan.ignore', '1'),
                           ('dhcp.lan.ra', 'disabled'), ('dhcp.lan.dhcpv6', 'disabled'),
                           ('network.br_lan.ports', 'lan1 lan2'), ('q1000k-xgspon.service.enabled', '0')]:
            self.assertEqual(fixture.uci_cmd('get', key), value)


if __name__ == '__main__':
    unittest.main()
