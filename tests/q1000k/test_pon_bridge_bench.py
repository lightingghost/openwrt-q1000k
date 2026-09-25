#!/usr/bin/env python3
"""Exercise evidence verdicts and the actual shell recovery transaction."""
import importlib.util
import json
import os
import pty
import re
import shutil
import signal
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bridge_bench', REPO / 'scripts/q1000k/bridge-bench.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class EvidenceTests(unittest.TestCase):
    def test_requires_matching_bidirectional_hardware_and_preserved_headers(self):
        for family in (4, 6):
            local, remote = ('192.0.2.2', '198.51.100.5') if family == 4 else ('2001:db8::2', '2001:db8:1::5')
            mac = '02:00:00:00:00:42'
            smac = mac if family == 4 else '00:0f:00:00:00:00'
            upstream, downstream = f'{local}:42000->{remote}:443', f'{remote}:443->{local}:42000'
            original = f'orig={upstream}' + (f' new={upstream}' if family == 4 else '')
            reply = f'orig={downstream}' + (f' new={downstream}' if family == 4 else '')
            up = f'00001 BND IPv{family} 5T {original} eth={smac}->02:00:00:00:00:01 etype=03ff data=007ffa00 vlan=122,0 ib1=20000365 ib2=0003e25f'
            down = f'00002 BND IPv{family} 5T {reply} eth={smac}->{mac} etype=8002 data=007f0800 vlan=0,0 ib1=20000365 ib2=0003e621'
            ct = f'tcp src={local} dst={remote} sport=42000 dport=443 [HW_OFFLOAD]'
            transfer = dict(local_ip=local, remote_ip=remote, local_port=42000, remote_port=443)
            samples = '\n'.join((up, down, ct))
            self.assertTrue(bench.assess(samples, [transfer], mac, True)['passed'])
            for broken in (up + '\n' + ct, samples.replace('[HW_OFFLOAD]', '[OFFLOAD]'),
                           samples.replace('ib1=20000365', 'ib1=21000365'),
                           samples.replace('etype=03ff', 'etype=0000'),
                           samples.replace('ib2=0003e621', 'ib2=0003e601'),
                           samples.replace('42000', '43000')):
                self.assertFalse(bench.assess(broken, [transfer], mac, True)['passed'])
            self.assertFalse(bench.assess(samples, [transfer], mac, False)['passed'])
            self.assertTrue(bench.assess(ct.replace('[HW_OFFLOAD]', ''), [transfer], mac, False)['passed'])
            if family == 4:
                broken = samples.replace('new=' + upstream, 'new=203.0.113.9:42000->198.51.100.5:443')
            else:
                broken = samples.replace('00:0f:00:00:00:00', '00:02:00:00:00:00')
            self.assertFalse(bench.assess(broken, [transfer], mac, True)['passed'])


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pon-bridge-recovery-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = dict(os.environ, TEST_ROOT=str(self.root), PATH=str(self.root / 'bin') + ':' + os.environ['PATH'])
        self.state = self.root / 'var/run/q1000k-pon-passthrough'
        self.state.mkdir(parents=True)
        for path in ('etc/config', 'etc/init.d', 'bin', 'tmp', 'sys/class/net/br-lan/brif',
                     'sys/firmware/devicetree/base'):
            (self.root / path).mkdir(parents=True, exist_ok=True)
        self.write('sys/firmware/devicetree/base/quantum,xgspon-activation-bench', '')
        self.write('etc/q1000k-private-autostart', '')
        self.originals = {}
        for config in ('network', 'dhcp', 'firewall'):
            original = ('original ' + config + '\n').encode()
            self.originals[config] = original
            (self.state / config).write_bytes(original)
            self.write('etc/config/' + config, 'test configuration\n')
        for name, value in (('saved', ''), ('active', ''), ('token', 'fixture'), ('deadline', '100'), ('management-neighbor', '192.0.2.2 02:00:00:00:00:42\n')):
            (self.state / name).write_text(value)
        tool = '''#!/bin/sh
printf '%s %s\n' "${0##*/}" "$*" >> "$TEST_ROOT/calls"
case "${0##*/}:$*" in
 network:reload) [ "$FAIL_RELOAD" != 1 ] || exit 1;;
 ip:'neigh show '*) echo "192.0.2.2 lladdr 02:00:00:00:00:42 PERMANENT";;
esac
exit 0
'''
        for name in ('network', 'dnsmasq', 'odhcpd', 'bridge-hw-offload'):
            self.write('etc/init.d/' + name, tool, executable=True)
        for name in ('uci', 'nft', 'ip'):
            self.write('bin/' + name, tool, executable=True)
        source = (REPO / 'package/network/utils/q1000k-xgspon-bench/files/passthrough').read_text()
        source = re.sub(r'/(?:var|sys|etc|proc|usr|tmp)/',
                        lambda match: str(self.root) + match.group(), source)
        source = re.sub(r'\bip (?=(?:neigh|link)\b)', str(self.root / 'bin/ip') + ' ', source)
        self.script = self.write('passthrough', source)

    def write(self, name, data, executable=False):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(data)
        if executable:
            path.chmod(0o755)
        return path

    def run_script(self, *args, **env):
        return subprocess.run(['/usr/bin/busybox', 'ash', str(self.script), *args],
                              env=self.env | env, text=True, capture_output=True, timeout=10)

    def test_restore_original_bytes_and_retire_before_topology_change(self):
        result = self.run_script('stop')
        self.assertEqual(result.returncode, 0, result.stderr)
        for name, content in self.originals.items():
            self.assertEqual((self.root / 'etc/config' / name).read_bytes(), content)
        self.assertFalse(self.state.exists())
        self.assertTrue((self.root / 'tmp/q1000k-pon-passthrough-finished-fixture/network').exists())
        calls = (self.root / 'calls').read_text()
        self.assertLess(calls.index('bridge-hw-offload stop'), calls.index('network reload'))
        self.assertLess(calls.index('network reload'), calls.index('nft destroy table bridge q1000k_pon_test'))

    def test_failed_restore_retains_independent_guard_and_backups(self):
        result = self.run_script('stop', FAIL_RELOAD='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.state / 'active').exists())
        self.assertTrue((self.state / 'network').exists())
        self.assertEqual(self.run_script('stop').returncode, 0)

    def test_unfinished_detach_keeps_provider_management_isolation(self):
        (self.root / 'sys/class/net/br-lan/brif/pon').mkdir()
        result = self.run_script('stop')
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.state / 'active').exists())
        self.assertNotIn('nft destroy table bridge q1000k_pon_test', (self.root / 'calls').read_text())

    def test_old_watchdog_cannot_revert_a_new_session(self):
        self.assertEqual(self.run_script('stop', 'old-token').returncode, 0)
        self.assertTrue((self.state / 'active').exists())
        self.assertFalse((self.root / 'calls').exists())

    def test_existing_test_and_invalid_client_are_rejected_before_mutation(self):
        for args in (('start', 'lan1', '02:00:00:00:00:42'),
                     ('start', 'lan1', '01:00:00:00:00:42'),
                     ('start', 'lan1', '02:00:00:00:00:42', '0'),
                     ('start', 'pon', '02:00:00:00:00:42')):
            self.assertNotEqual(self.run_script(*args).returncode, 0)
            self.assertTrue((self.state / 'active').exists())
        self.assertFalse((self.root / 'calls').exists())

    def test_actual_openwrt_config_library_under_strict_shell(self):
        shutil.rmtree(self.state)
        for path in ('sys/class/net/br-lan/brif/lan1', 'sys/class/net/pon'):
            (self.root / path).mkdir(parents=True)
        self.write('sys/class/net/br-lan/bridge/vlan_filtering', '0\n')
        self.write('bin/uci', '''#!/bin/sh
case "$*" in
 '-q get network.lan.device') echo br-lan;;
 '-q get network.lan.netmask') echo 255.255.255.0;;
 '-q get network.lan.ipaddr') echo 192.168.0.1;;
 *'export network') printf "config device 'fixture_bridge'\\noption name 'br-lan'\\n";;
esac
''', executable=True)
        (self.root / 'sbin').mkdir()
        (self.root / 'sbin/uci').symlink_to(self.root / 'bin/uci')
        for source, dest in (
            ('package/base-files/files/lib/functions.sh', 'lib/functions.sh'),
            ('package/system/uci/files/lib/config/uci.sh', 'lib/config/uci.sh')):
            data = (REPO / source).read_text()
            data = re.sub(r'/(?:lib|sbin|var)/', lambda m: str(self.root) + m.group(), data)
            self.write(dest, data)
        self.script.write_text(self.script.read_text().replace('/lib/functions.sh', str(self.root / 'lib/functions.sh')))
        for name in ('bridge', 'tcpdump', 'ip', 'jsonfilter', 'q1000k-omci'):
            self.write('bin/' + name, '#!/bin/sh\nexit 0\n', executable=True)
        result = self.run_script('start', 'lan1', '02:00:00:00:00:42', '120',
                                 '00000000-0000-0000-0000-000000000001', '192.168.0.2', '02:00:00:00:00:42')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('PON is not authenticated', result.stderr)
        self.assertNotIn('parameter not set', result.stderr)
        self.assertFalse(self.state.exists())

    def test_guard_survives_hangup_and_restores_without_collector(self):
        self.write('proc/uptime', '100.00 0.00\n')
        (self.state / 'deadline').write_text('1000\n')
        script = self.script.read_text().replace(str(self.root / 'usr/sbin/q1000k-pon-passthrough'), str(self.script))
        self.script.write_text(script)
        self.script.chmod(0o755)
        with (self.root / 'guard.log').open('wb') as log:
            guard = subprocess.Popen(['/usr/bin/busybox', 'ash', self.script, 'guard', 'fixture'],
                                     env=self.env, stdout=log, stderr=log, start_new_session=True)
            try:
                deadline = time.monotonic() + 3
                while not (self.state / 'guard.ready').exists() and time.monotonic() < deadline:
                    time.sleep(.01)
                self.assertEqual((self.state / 'guard.ready').read_text().strip(), 'fixture')
                guard.send_signal(signal.SIGHUP)
                time.sleep(.05)
                self.assertIsNone(guard.poll())
                (self.state / 'deadline').write_text('0\n')
                self.assertEqual(guard.wait(timeout=6), 0)
                self.assertFalse(self.state.exists())
                for name, content in self.originals.items():
                    self.assertEqual((self.root / 'etc/config' / name).read_bytes(), content)
            finally:
                if guard.poll() is None:
                    guard.kill()
                    guard.wait()

    def test_heartbeat_token_and_hard_deadline(self):
        self.write('proc/uptime', '100.00 0.00\n')
        (self.state / 'hard-deadline').write_text('150\n')
        self.assertNotEqual(self.run_script('heartbeat', 'wrong').returncode, 0)
        self.assertEqual((self.state / 'deadline').read_text(), '100')
        result = self.run_script('heartbeat', 'fixture')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.state / 'deadline').read_text().strip(), '150')


class HostRecoveryTests(unittest.TestCase):
    def test_competing_dhcp_client_on_selected_interface_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for pid, argv in ((1, ['dhclient', '-v', 'eth_fixture']), (2, ['NetworkManager', '--no-daemon']),
                              (3, ['udhcpc', '-i', 'eth_fixture'])):
                (root / str(pid)).mkdir()
                (root / str(pid) / 'cmdline').write_bytes(('\0'.join(argv) + '\0').encode())
            self.assertEqual([p['pid'] for p in bench.competing_dhcp_clients('eth_fixture', root)], [1, 3])

    def test_host_timer_is_renewed_before_onu_and_failure_stops_renewals(self):
        calls = []
        host = mock.Mock()
        host.renew.side_effect = lambda name: calls.append('host')
        beat = bench.Heartbeat(host, lambda *args, **kw: calls.append('onu'), 'token')
        beat.beat(0)
        self.assertEqual(calls, ['host', 'onu'])
        host.renew.side_effect = RuntimeError('NM disconnected')
        with self.assertRaises(RuntimeError):
            beat.beat(1)
        self.assertEqual(calls, ['host', 'onu'])

    def test_separate_management_link_and_checkpoint_recovery(self):
        commands = []
        def run(command, name, **kwargs):
            commands.append((name, command))
            data = {
                'management-route-before': b'[]', 'management-addresses-before': b'[]',
                'checkpoint-device': b'{"data":["/org/freedesktop/NetworkManager/Devices/2"]}',
                'checkpoint-create': b'{"data":["/org/freedesktop/NetworkManager/Checkpoint/1"]}',
                'checkpoint-rollback': b'{"data":[{"/org/freedesktop/NetworkManager/Devices/2":0}]}',
                'restore-client-state': b'original\n100 (connected)\n',
            }.get(name, b'')
            return subprocess.CompletedProcess(command, 0, data, b'')
        host = bench.HostRecovery(run, 'physical', 'original', '192.0.2.1', '192.0.2.2', '12345678')
        host.prepare()
        self.assertTrue(host.link_created)
        self.assertEqual(dict(commands)['management-address'][-1], host.link)
        self.assertNotIn('physical', dict(commands)['management-address'])
        # Simulate DHCP failure. The dedicated management interface is retained
        # through checkpoint rollback and removed only by the separate call.
        host.restore()
        self.assertTrue(host.link_created)
        host.remove_management()
        self.assertFalse(host.link_created)
        self.assertLess([n for n, _ in commands].index('checkpoint-rollback'),
                        [n for n, _ in commands].index('management-delete'))

    def test_serial_recovery_executes_checked_command_and_rejects_identity_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / 'serial.log'
            log.touch()
            master, slave = pty.openpty()
            shell = subprocess.Popen(['/usr/bin/busybox', 'ash', '-i'], stdin=slave, stdout=slave, stderr=slave)
            os.close(slave)
            def reader():
                with log.open('ab', buffering=0) as stream:
                    try:
                        while True:
                            stream.write(os.read(master, 4096))
                    except OSError:
                        pass
            thread = threading.Thread(target=reader, daemon=True)
            thread.start()
            device = mock.Mock()
            device.open.side_effect = lambda *a, **kw: os.fdopen(os.dup(master), 'wb', buffering=0)
            serial = bench.SerialRecovery(device, log, root, 'set -eu\ntest fixture = fixture\n')
            try:
                target = root / 'restored'
                serial.execute('touch ' + str(target), 'restore', timeout=3)
                self.assertTrue(target.exists())
                target.unlink()
                serial.identity = 'set -eu\ntest wrong = fixture\n'
                with self.assertRaisesRegex(RuntimeError, 'identity or command failed'):
                    serial.execute('touch ' + str(target), 'wrong-boot', timeout=3)
                self.assertFalse(target.exists())
            finally:
                shell.kill()  # Interactive ash intentionally ignores SIGTERM.
                shell.wait(timeout=3)
                os.close(master)
                thread.join(timeout=1)

    def run_failed_dhcp(self, serial_fails=False):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name) / 'build-artifacts'
        root.mkdir()
        output = root / 'run'
        manifest = root / 'manifest.json'
        manifest.write_text(json.dumps(dict(kernel_notes_sha256='a' * 64, runtime_sha256={})))
        calls = []
        original = '00000000-0000-0000-0000-000000000001'
        boot = '00000000-0000-0000-0000-000000000002'
        def run(command, **kwargs):
            calls.append(command)
            data, status = b'', 0
            if command[0] == 'ssh':
                remote = command[-1]
                if remote.endswith('cat /proc/sys/kernel/random/boot_id'):
                    data = boot.encode()
                elif remote.endswith('sha256sum /etc/config/network /etc/config/dhcp /etc/config/firewall'):
                    data = ''.join('b' * 64 + '  /etc/config/' + n + '\n' for n in ('network', 'dhcp', 'firewall')).encode()
                elif bench.TOOL + ' stop ' in remote:
                    status = 255  # Force the serial fallback after DHCP failure.
                elif serial_fails and 'test ! -e ' + bench.STATE + '/active' in remote:
                    status = 255
            elif command[0] == 'nmcli':
                if command[1:3] == ['-g', 'GENERAL.CON-UUID']:
                    data = original.encode()
                elif 'GENERAL.CON-UUID,GENERAL.STATE' in command:
                    data = (original + '\n100 (connected)\n').encode()
                elif 'up' in command and any(s.startswith('q1000k-bridge-') for s in command):
                    status = 10
            elif command[0] == 'ip' and '-j' in command:
                data = b'[]'
            elif command[0] == 'busctl' and '--json=short' in command:
                if 'GetDeviceByIpIface' in command:
                    data = b'{"data":["/org/freedesktop/NetworkManager/Devices/2"]}'
                elif 'CheckpointCreate' in command:
                    data = b'{"data":["/org/freedesktop/NetworkManager/Checkpoint/1"]}'
                elif 'CheckpointRollback' in command:
                    data = b'{"data":[{"/org/freedesktop/NetworkManager/Devices/2":0}]}'
            return subprocess.CompletedProcess(command, status, data, b'')
        def serial(command, name, **kwargs):
            calls.append(['serial', name, command])
            if serial_fails and name != 'serial-preflight':
                raise RuntimeError('Serial recovery unavailable')
        read_text = Path.read_text
        def read(path, *args, **kwargs):
            if str(path).startswith('/sys/class/net/') and path.name == 'address':
                return '02:00:00:00:00:42\n'
            return read_text(path, *args, **kwargs)
        argv = ['bridge-bench', '--interface', 'fixture0', '--port', 'lan1',
                '--known-hosts', str(root / 'known_hosts'), '--manifest', str(manifest),
                '--output', str(output), '--serial-device', '/dev/null', '--serial-log', str(root / 'serial.log')]
        real_open = open
        def fixture_open(path, *args, **kwargs):
            if str(path).startswith('/run/lock/'):
                path = root / 'lock'
            return real_open(path, *args, **kwargs)
        with mock.patch.object(bench.subprocess, 'run', side_effect=run), \
             mock.patch.object(bench.os, 'geteuid', return_value=0), \
             mock.patch.object(bench.shutil, 'which', return_value='/fixture/program'), \
             mock.patch.object(bench, 'competing_dhcp_clients', return_value=[]), \
             mock.patch.object(bench.SerialRecovery, 'execute', side_effect=serial), \
             mock.patch.object(bench.signal, 'signal'), \
             mock.patch('sys.argv', argv), mock.patch.object(Path, 'read_text', read), \
             mock.patch('builtins.open', side_effect=fixture_open):
            with self.assertRaisesRegex(RuntimeError, 'client-dhcp failed'):
                bench.main()
        return json.loads((output / 'summary.json').read_text()), calls

    def test_failed_dhcp_uses_serial_then_restores_host_and_removes_management(self):
        summary, calls = self.run_failed_dhcp()
        self.assertFalse(summary['passed'])
        self.assertFalse(summary['completed'])
        self.assertEqual(summary['cleanup_errors'], [])
        self.assertIsNone(summary['retained_management_link'])
        serial = next(i for i, c in enumerate(calls) if c[:2] == ['serial', 'serial-restore'])
        rollback = next(i for i, c in enumerate(calls) if 'CheckpointRollback' in c)
        removal = next(i for i, c in enumerate(calls) if c[:3] == ['ip', 'link', 'delete'])
        self.assertLess(serial, rollback)
        self.assertLess(rollback, removal)

    def test_failed_router_restore_retains_management_and_host_checkpoint(self):
        summary, calls = self.run_failed_dhcp(serial_fails=True)
        self.assertFalse(summary['passed'])
        self.assertFalse(summary['router_restored'])
        self.assertTrue(summary['cleanup_errors'])
        self.assertIsNotNone(summary['retained_management_link'])
        self.assertIsNotNone(summary['checkpoint'])
        self.assertFalse(any('CheckpointRollback' in c or c[:3] == ['ip', 'link', 'delete'] for c in calls))


if __name__ == '__main__':
    unittest.main()
