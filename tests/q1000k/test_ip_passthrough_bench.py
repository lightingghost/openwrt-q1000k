#!/usr/bin/env python3
"""Validate routed PPE evidence and failure-aware RAM transaction recovery."""
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'scripts/q1000k'))
spec = importlib.util.spec_from_file_location('bench', REPO / 'scripts/q1000k/ip-passthrough-bench.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)

class EvidenceTests(unittest.TestCase):
    def test_requires_routed_bidirectional_unchanged_tuple_hardware_evidence(self):
        client, onu = '02:00:00:00:00:42', '02:00:00:00:00:01'
        up = '198.51.100.2:42000->203.0.113.5:443'
        down = '203.0.113.5:443->198.51.100.2:42000'
        sample = (f'00001 BND IPv4 5T orig={up} new={up} eth={onu}->02:00:00:00:00:02 etype=03ff data=007ffa00 vlan=122,0 ib1=21000365 ib2=0003e25f\n'
                  f'00002 BND IPv4 5T orig={down} new={down} eth={onu}->{client} etype=8002 data=007f0800 vlan=0,0 ib1=21000365 ib2=0003e621\n'
                  'tcp src=198.51.100.2 dst=203.0.113.5 sport=42000 dport=443 [HW_OFFLOAD]\n')
        transfer = dict(local_ip='198.51.100.2', local_port=42000, remote_ip='203.0.113.5', remote_port=443)
        check = lambda text, hw=True: bench.assess(text, [transfer], client, hw, onu)['passed']
        self.assertTrue(check(sample))
        for broken in (sample.splitlines()[0], sample.replace('[HW_OFFLOAD]', '[OFFLOAD]'),
                       sample.replace('ib1=21000365', 'ib1=20000365'),
                       sample.replace('etype=03ff', 'etype=0000'),
                       sample.replace('ib2=0003e621', 'ib2=0003e601'),
                       sample.replace('42000', '43000'),
                       sample.replace('new=' + up, 'new=192.0.2.2:42000->203.0.113.5:443'),
                       sample.replace('eth=' + onu, 'eth=' + client),
                       sample.replace('->' + client, '->02:00:00:00:00:99')):
            self.assertFalse(check(broken), broken)
        self.assertFalse(check(sample, False))
        self.assertTrue(check(sample.splitlines()[-1].replace('[HW_OFFLOAD]', '[OFFLOAD]'), False))

class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='q1000k-l3-recovery-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = dict(os.environ, TEST_ROOT=str(self.root), PATH=str(self.root / 'bin') + ':' + os.environ['PATH'])
        self.state = self.root / 'var/run/q1000k-ip-passthrough'
        self.state.mkdir(parents=True)
        self.original = {}
        for cfg in ('network', 'dhcp', 'firewall'):
            content = 'original ' + cfg + '\n'
            self.original[cfg] = content
            (self.state / cfg).write_text(content)
            self.write('etc/config/' + cfg, content if cfg == 'network' else 'changed\n')
        for name, value in dict(saved='', active='', token='fixture', deadline='9', wan='198.51.100.2',
                                **{'routing-started':'', 'rules-before':'0: from all lookup local\n32766: from all lookup main\n32767: from all lookup default\n',
                                   'management-neighbor':'192.0.2.2 02:00:00:00:00:42\n'}).items():
            (self.state / name).write_text(value)
        self.extra = self.write('tmp/dnsmasq.fixture.d/q1000k-ip-passthrough.conf', 'test DHCP configuration\n')
        (self.state / 'dnsmasq.extra').write_text(self.extra.read_text())
        (self.state / 'dhcp-file').write_text(str(self.extra) + '\n')
        self.unrelated = self.write('tmp/dnsmasq.fixture.d/extraconfig.conf', 'existing custom configuration\n')
        self.write('proc/uptime', '10.0 0.0\n')
        self.write('proc/sys/net/ipv4/conf/br-lan/accept_local', '1\n')
        (self.state / 'sysctls').write_text(str(self.root / 'proc/sys/net/ipv4/conf/br-lan/accept_local') + ' 0\n')
        self.write('sys/firmware/devicetree/base/quantum,xgspon-activation-bench', '')
        self.write('etc/q1000k-private-autostart', '')
        (self.root / 'tmp').mkdir(exist_ok=True)
        generic = '''#!/bin/sh
printf '%s %s\n' "${0##*/}" "$*" >> "$TEST_ROOT/calls"
case "${0##*/}:$*" in
 firewall:reload) [ "$FAIL_RELOAD" != 1 ] || exit 1;;
 ip:'-4 rule show') cat "$TEST_ROOT/var/run/q1000k-ip-passthrough/rules-before";;
 ip:'-4 route show table 100') [ "$FAIL_ROUTE" != 1 ] || echo '198.51.100.2 dev br-lan';;
esac
exit 0
'''
        for name in ('ip', 'uci', 'nft'):
            self.write('bin/' + name, generic, True)
        for name in ('firewall', 'dnsmasq'):
            self.write('etc/init.d/' + name, generic, True)
        source = (REPO / 'package/network/utils/q1000k-xgspon-bench/files/ip-passthrough').read_text()
        source = re.sub(r'/(?:var|sys|etc|proc|usr|tmp)/', lambda m: str(self.root) + m.group(), source)
        # Host BusyBox prefers its built-in ip applet even before PATH stubs.
        source = re.sub(r'\bip (?=(?:-4|neigh|route|address|link)\b)', str(self.root / 'bin/ip') + ' ', source)
        self.script = self.write('usr/sbin/q1000k-ip-passthrough', source, True)

    def write(self, name, text, executable=False):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        if executable:
            path.chmod(0o755)
        return path

    def run_helper(self, *args, **env):
        return subprocess.run(['busybox', 'ash', str(self.script), *args], env=self.env | env,
                              capture_output=True, text=True, timeout=10)

    def test_restore_then_release_management_pin(self):
        result = self.run_helper('stop', 'fixture')
        self.assertEqual(result.returncode, 0, result.stderr)
        for cfg, original in self.original.items():
            self.assertEqual((self.root / 'etc/config' / cfg).read_text(), original)
        self.assertFalse(self.state.exists())
        self.assertFalse(self.extra.exists())
        self.assertEqual(self.unrelated.read_text(), 'existing custom configuration\n')
        self.assertEqual((self.root / 'proc/sys/net/ipv4/conf/br-lan/accept_local').read_text(), '0\n')
        calls = (self.root / 'calls').read_text()
        self.assertIn('ip neigh replace 192.0.2.2', calls)
        self.assertNotIn('ip neigh del 192.0.2.2', calls)
        self.assertEqual(self.run_helper('management-release', 'fixture').returncode, 0)
        self.assertIn('ip neigh del 192.0.2.2', (self.root / 'calls').read_text())

    def test_failed_restore_retains_guard_and_backup_until_retry(self):
        for reason in ('FAIL_RELOAD', 'FAIL_ROUTE'):
            result = self.run_helper('stop', 'fixture', **{reason:'1'})
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue((self.state / 'active').exists())
            self.assertTrue((self.state / 'firewall').exists())
        self.assertEqual(self.run_helper('stop', 'fixture').returncode, 0)

    def test_wrong_token_cannot_restore_active_transaction(self):
        self.assertEqual(self.run_helper('stop', 'other').returncode, 0)
        self.assertTrue((self.state / 'active').exists())
        self.assertFalse((self.root / 'calls').exists())

    def test_expired_guard_restores_without_host(self):
        result = self.run_helper('guard', 'fixture')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.state.exists())
        self.assertTrue((self.root / 'tmp/q1000k-ip-passthrough-finished-fixture/firewall').exists())

if __name__ == '__main__':
    unittest.main()
