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

    def test_cpu_policy_discovery_and_controls(self):
        for app in self.scripts:
            self.assertFalse(self.rpc(app, 'getStatus')['cpu_policy_available'])
        for method, args in [('setGovernor', {'governor':'performance'}), ('setMaxFreq', {'freq':500000})]:
            self.assertIn('has not registered a policy', self.rpc('npu', method, args)['error'])
        # Policy ID is not guaranteed to be zero. cpuinfo_cur_freq may be absent.
        base = '/sys/devices/system/cpu/cpufreq/policy2/'
        for name, value in {'scaling_cur_freq':'750000', 'scaling_min_freq':'500000',
                            'scaling_max_freq':'1200000', 'scaling_governor':'ondemand',
                            'scaling_available_governors':'ondemand performance',
                            'stats/time_in_state':'500000 1\n750000 2\n1200000 3'}.items():
            self.write(base+name, value+'\n')
        for app in self.scripts:
            status = self.rpc(app, 'getStatus')
            self.assertTrue(status['cpu_policy_available'])
            self.assertEqual(status['cpu_cur_freq'], 750000)
            self.assertEqual(status['cpu_hw_freq'], 0)
            self.assertEqual(status['cpu_avail_freqs'].split(), ['500000','750000','1200000'])
            self.assertEqual(status['cpu_governor'], 'ondemand')
        self.assertEqual(self.rpc('npu', 'setGovernor', {'governor':'performance'}),
                         {'result':'ok','governor':'performance'})
        self.assertEqual(self.rpc('npu', 'setMaxFreq', {'freq':750000}), {'result':'ok','freq':750000})
        self.write(base+'scaling_min_freq', '750000\n')
        self.assertIn('below the current minimum', self.rpc('npu', 'setMaxFreq', {'freq':500000})['error'])
        self.assertEqual((self.root / (base+'scaling_max_freq').lstrip('/')).read_text(), '750000\n')
        # Prefer CPU0's policy link/path if it exists.
        self.write('/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq', '1000000\n')
        for app in self.scripts:
            self.assertEqual(self.rpc(app, 'getStatus')['cpu_cur_freq'], 1000000)

    def test_ppe_hex_indices_and_counts(self):
        bind = 'a000 BND IPv4 5T orig=192.0.2.1:1->192.0.2.2:2 eth=aa:bb:cc:dd:ee:ff->11:22:33:44:55:66\n'
        self.write('/sys/kernel/debug/ppe/bind', bind)
        self.write('/sys/kernel/debug/ppe/entries', bind + 'ffff UNB IPv4 5T orig=192.0.2.3:1->192.0.2.4:2\n')
        ppe = self.rpc('flowsense', 'getPpeEntries')
        self.assertEqual(ppe['bnd']['total'], 1)
        self.assertEqual(ppe['unb']['total'], 1)
        self.assertEqual(self.rpc('flowsense', 'getNpuBypass')['offload_bound'], 1)

    def test_pse_buffer_snapshot(self):
        data = self.write('/sys/kernel/debug/ppe/pse',
                          'version 1\ntotal 2048\nreserved 512\nused 300\nfree 1200\nhigh 1504\n')
        actual = self.rpc('flowsense', 'getFrameEngine')
        self.assertTrue(actual['available'])
        self.assertEqual(actual['source'], 'driver-pse')
        self.assertEqual(actual['pse_used'], 300)
        self.assertEqual(actual['pse_free'], 1200)
        good = data.read_text()
        for broken in (good.replace('version 1', 'version 3'), good.replace('free 1200', 'free -1'),
                       good.replace('free 1200', 'free 999999'), good.replace('high 1504', 'high 0'),
                       good.replace('total 2048', 'total broken'), good.replace('used 300\n', ''),
                       good + 'used 300\n'):
            data.write_text(broken)
            self.assertFalse(self.rpc('flowsense', 'getFrameEngine')['available'], broken)
        data.write_text(good.replace('free 1200', 'free 0'))
        self.assertEqual(self.rpc('flowsense', 'getFrameEngine')['pse_free'], 0)

    def test_pse_cdm_drop_snapshot(self):
        occupancy = 'version 2\ntotal 2048\nreserved 512\nused 0\nfree 1536\nhigh 1504\n'
        counters = ''.join(f'pse_drop{i} {i}\n' for i in range(10))
        counters += 'cdm1_hwf_drop 4294967295\ncdm2_hwf_drop 12\n'
        good = occupancy + counters
        path = self.write('/sys/kernel/debug/ppe/pse', good)
        value = self.rpc('flowsense','getFrameEngine')
        self.assertTrue(value['drops_available'])
        self.assertEqual([p['drops'] for p in value['pse_ports']], list(range(10)))
        self.assertEqual(value['cdm1']['rx_hwf_drop'], 4294967295)
        self.assertEqual(value['cdm2']['rx_hwf_drop'], 12)
        for broken in (good.replace('pse_drop0 0', 'pse_drop0 4294967296'),
                       good.replace('pse_drop9 9\n',''), good+'pse_drop2 2\n',
                       good.replace('cdm2_hwf_drop 12','cdm2_hwf_drop -1'),
                       good.replace('cdm1_hwf_drop 4294967295','cdm1_hwf_drop invalid')):
            path.write_text(broken)
            value = self.rpc('flowsense','getFrameEngine')
            self.assertTrue(value['available']) # occupancy remains usable
            self.assertFalse(value['drops_available'])
        path.write_text(occupancy.replace('version 2','version 1'))
        value = self.rpc('flowsense','getFrameEngine')
        self.assertTrue(value['available'])
        self.assertFalse(value['drops_available'])
        self.assertIn('updated Q1000K kernel', value['drop_error'])

    def test_ppe_source_availability(self):
        missing = self.rpc('flowsense','getPpeEntries')
        self.assertFalse(missing['bnd']['available'])
        self.assertFalse(missing['unb']['available'])
        self.write('/sys/kernel/debug/ppe/bind', '')
        self.write('/sys/kernel/debug/ppe/entries', '')
        empty = self.rpc('flowsense','getPpeEntries')
        self.assertTrue(empty['bnd']['available'])
        self.assertTrue(empty['unb']['available'])
        self.assertEqual(empty['bnd']['total'] + empty['unb']['total'], 0)

    def test_ethernet_integrity_data(self):
        for iface, carrier, speed in [('lan1', '1', '1000'), ('lan2', '0', '-1')]:
            self.write(f'/sys/class/net/{iface}/carrier', carrier)
            self.write(f'/sys/class/net/{iface}/speed', speed)
            for field, value in [('rx_bytes', 1234), ('tx_bytes', 2345), ('rx_errors', 3),
                                 ('tx_errors', 0), ('rx_crc_errors', 2), ('rx_dropped', 1), ('tx_dropped', 4)]:
                self.write(f'/sys/class/net/{iface}/statistics/{field}', str(value))
        ports = self.rpc('flowsense', 'getEthStats')['ports']
        self.assertEqual([p['iface'] for p in ports], ['lan1', 'lan2'])
        self.assertTrue(ports[0]['stats_available'])
        self.assertTrue(ports[0]['up'])
        self.assertFalse(ports[1]['up'])
        self.assertEqual(ports[0]['rx_crc_errors'], 2)
        self.write('/sys/class/net/lan1/statistics/rx_errors', 'unavailable')
        self.assertFalse(self.rpc('flowsense', 'getEthStats')['ports'][0]['stats_available'])

    def test_latency_result_freshness(self):
        self.write('/proc/uptime', '100.00 20.00\n')
        self.assertEqual(self.rpc('flowsense', 'getJitterResult')['state'], 'stopped')
        sample = {'updated': 99, 'state': 'ok', 'available': True, 'reachable': True, 'last_ping': 0}
        self.write('/tmp/npu-jitter.json', json.dumps(sample))
        self.assertEqual(self.rpc('flowsense', 'getJitterResult'), sample)
        for updated in (1, 101, 'broken'):
            sample['updated'] = updated
            self.write('/tmp/npu-jitter.json', json.dumps(sample))
            self.assertEqual(self.rpc('flowsense', 'getJitterResult')['state'], 'stale')

    def test_latency_target_configuration(self):
        commands = self.root / 'commands'
        commands.mkdir()
        db = self.write('/uci.json', json.dumps({'npu-monitor.@jitter[0]': 'jitter',
                                               'npu-monitor.@jitter[0].target': '192.0.2.1'}))
        log = self.write('/uci-calls', '')
        uci = commands / 'fixture_uci'
        uci.write_text('#!/usr/bin/python3\n' + f'''import json, sys
from pathlib import Path
p=Path({str(db)!r}); d=json.loads(p.read_text()); args=[a for a in sys.argv[1:] if a!="-q"]
with open({str(log)!r}, 'a') as f: f.write(json.dumps(args)+"\\n")
if args[0]=='get':
    if args[1] not in d: sys.exit(1)
    print(d[args[1]])
elif args[0]=='set':
    k,v=args[1].split('=',1); d[k]=v; p.write_text(json.dumps(d))
elif args[0]!='commit': sys.exit(1)
''')
        uci.chmod(0o755)
        self.scripts['flowsense'].write_text(self.scripts['flowsense'].read_text().replace('uci ', 'fixture_uci '))
        self.env['PATH'] = str(commands) + ':' + self.env['PATH']
        service_log = self.root / 'service-calls'
        service = self.write('/etc/init.d/npu-jitter', f'#!/bin/sh\necho "$1" >> "{service_log}"\n')
        service.chmod(0o755)
        self.assertEqual(self.rpc('flowsense', 'getLatencyConfig')['target'], '192.0.2.1')
        for target in ('fe80::1%br-lan', '', 'example.net'):
            self.assertEqual(self.rpc('flowsense', 'setLatencyTarget', {'target':target}), {'result':'ok'})
            self.assertEqual(self.rpc('flowsense', 'getLatencyConfig')['target'], target)
            self.assertNotIn('npu-monitor.jitter', json.loads(db.read_text()))
        self.assertEqual(service_log.read_text().splitlines(), ['enable','restart'] * 3)
        before = db.read_text()
        for args in ({}, {'target':3}, {'target':'-f'}, {'target':'a;id'}, {'target':'a\nb'}, {'target':'host\n'}, {'target':'a'*254}):
            self.assertIn('error', self.rpc('flowsense', 'setLatencyTarget', args))
        self.assertEqual(db.read_text(), before)
        service.write_text('#!/bin/sh\nexit 1\n')
        self.assertIn('error', self.rpc('flowsense', 'setLatencyTarget', {'target':'192.0.2.9'}))
        # An absent config gets a named section.
        db.write_text('{}')
        service.write_text('#!/bin/sh\nexit 0\n')
        self.assertEqual(self.rpc('flowsense', 'setLatencyTarget', {'target':''}), {'result':'ok'})
        self.assertEqual(json.loads(db.read_text())['npu-monitor.jitter'], 'jitter')

    def daemon(self, route4='', route6='', target='', replies=('2.0',), next_route4=None):
        source = REPO / 'package/luci-app-airoha-flowsense/root/usr/libexec/npu-jitter-daemon'
        script = source.read_text().replace('while true; do', 'for iteration in ' + ' '.join(map(str, range(len(replies)))) + '; do')
        script = re.sub(r'(?<![a-zA-Z0-9])/(proc|tmp)/', lambda m: f'{self.root}/{m[1]}/', script)
        script = script.replace('ip -4 route', 'fixture_ip -4 route').replace('ip -6 route', 'fixture_ip -6 route')
        script = script.replace('/usr/bin/ping', 'fixture_ping')
        history = self.root / 'history'
        tick = f'cat "$RESULT_FILE" >> "{history}"'
        if next_route4:
            self.write('/next-route4', next_route4)
            tick += f'\ncp "{self.root}/next-route4" "{self.root}/route4"'
        script = script.replace('sleep "$INTERVAL"', tick)
        self.write('/proc/uptime', '100.00 20.00\n')
        self.write('/proc/stat', 'cpu  10 0 5 100 2 0 0 0 0 0\n')
        self.write('/tmp/placeholder', '')
        self.write('/route4', route4)
        self.write('/route6', route6)
        self.write('/replies', '\n'.join(replies) + '\n')
        self.write('/counter', '0')
        self.write('/ping-calls', '')
        history.write_text('')
        commands = self.root / 'commands'
        commands.mkdir(exist_ok=True)
        ip = commands / 'fixture_ip'
        ip.write_text(f'#!/bin/sh\ncase "$1" in -4) cat "{self.root}/route4";; -6) cat "{self.root}/route6";; esac\n')
        ip.chmod(0o755)
        ping = commands / 'fixture_ping'
        ping.write_text(f'''#!/bin/sh
printf '%s\\n' "$*" >> '{self.root}/ping-calls'
n=$(cat '{self.root}/counter'); n=$((n+1)); echo "$n" > '{self.root}/counter'
r=$(sed -n "${{n}}p" '{self.root}/replies')
[ "$r" = x ] && exit 1
printf '64 bytes from fixture: icmp_seq=1 ttl=64 time=%s ms\\n' "$r"
''')
        ping.chmod(0o755)
        daemon = self.root / 'daemon.sh'
        daemon.write_text(script)
        env = dict(self.env, PATH=str(commands) + ':' + self.env['PATH'])
        result = subprocess.run(['busybox', 'ash', str(daemon), target], env=env, capture_output=True, text=True)
        return result, [json.loads(line) for line in history.read_text().splitlines()]

    def test_latency_gateway_loss_and_jitter(self):
        result, samples = self.daemon(route4='default via 192.0.2.1 dev br-lan', replies=('2', 'x', '4'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(samples[0]['target'], '192.0.2.1')
        self.assertTrue(samples[0]['reachable'])
        self.assertEqual(samples[1]['state'], 'unreachable')
        self.assertIsNone(samples[1]['last_ping'])
        self.assertEqual(samples[2]['samples'], 2)
        self.assertEqual(samples[2]['attempts'], 3)
        self.assertAlmostEqual(samples[2]['loss'], 33.3)
        self.assertEqual(samples[2]['jitter'], 1)

    def test_latency_ipv6_custom_and_no_route(self):
        result, samples = self.daemon()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(samples[0]['state'], 'no_target')
        self.assertEqual((self.root / 'ping-calls').read_text(), '')
        result, samples = self.daemon(route6='default via fe80::1 dev br-lan proto ra', replies=('0',))
        self.assertEqual(samples[0]['target'], 'fe80::1%br-lan')
        self.assertEqual(samples[0]['last_ping'], 0)
        self.assertIn('-6', (self.root / 'ping-calls').read_text())
        result, samples = self.daemon(target='192.0.2.9', replies=('0.123',))
        self.assertEqual(samples[0]['last_ping'], .123)
        self.assertEqual(samples[0]['target'], '192.0.2.9')
        result, samples = self.daemon(target="host'; exit 0")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(samples)

    def test_latency_gateway_change(self):
        result, samples = self.daemon(route4='default via 192.0.2.1 dev br-lan',
                                     next_route4='default via 192.0.2.2 dev br-lan', replies=('2', '20'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([s['target'] for s in samples], ['192.0.2.1', '192.0.2.2'])
        self.assertEqual(samples[-1]['samples'], 1)
        self.assertEqual(samples[-1]['jitter'], 0)

    def test_latency_window(self):
        result, samples = self.daemon(target='192.0.2.1', replies=('2',) + ('x',) * 10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(samples[-1]['samples'], 0)
        self.assertEqual(samples[-1]['attempts'], 10)
        self.assertEqual(samples[-1]['loss'], 100)


if __name__ == '__main__':
    unittest.main()
