#!/usr/bin/env python3
"""Actual Linux routing/nft + actual dnsmasq/jsonfilter, in an EMPTY netns only.

Run: unshare -Urn python3 tests/q1000k/test_passthrough_integration.py
PT_JSONFILTER must identify a host build of the pinned jsonfilter source.
OpenWrt UCI/netifd events and init-service calls are fixtures; no host daemons,
host interfaces, optical devices or live ONU are touched.
"""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PKG = ROOT / 'package/network/utils/q1000k-passthrough/files'
MAC = '02:11:22:33:44:55'


def cmd(*args, **kw):
    return subprocess.run(args, text=True, capture_output=True, check=True, **kw).stdout


class NetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Refuse even to configure dummy devices in a host or existing bench ns.
        links = json.loads(cmd('ip', '-j', 'link', 'show'))
        if [link['ifname'] for link in links] != ['lo']:
            raise RuntimeError('Run only in a new unshare -Urn namespace')
        mapping = Path('/proc/self/uid_map').read_text().split()
        if len(mapping) != 3 or mapping[-1] != '1':
            raise RuntimeError('An isolated single-user mapping is required')
        cls.jsonfilter = Path(os.environ['PT_JSONFILTER']).resolve(strict=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='q1000k-pt-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ('bin', 'state', 'dnsmasq.test.d'):
            (self.root/name).mkdir()
        self.env = dict(os.environ, PT_TEST_ROOT=str(self.root),
                        PATH=str(self.root/'bin')+':'+os.environ['PATH'])
        (self.root/'bin/jsonfilter').symlink_to(self.jsonfilter)
        self.write('bin/ifstatus', '#!/bin/sh\ncat "$PT_TEST_ROOT/wan.json"\n', True)
        self.write('bin/conntrack', '#!/bin/sh\nprintf "%s\\n" "$*" >> "$PT_TEST_ROOT/conntrack.calls"\n', True)
        self.write('bin/dnsmasq-control', '#!/bin/sh\necho restart >> "$PT_TEST_ROOT/restarts"\n[ ! -f "$PT_TEST_ROOT/fail-restart" ]\n', True)
        self.write('bin/uci', '''#!/bin/sh
case "$*" in
 '-q get network.lan.device') echo br-lan;;
 '-q -X show dhcp') echo dhcp.test=dnsmasq;;
 *) exit 1;;
esac
''', True)
        text = (PKG/'ipv4').read_text().replace('STATE=/var/run/q1000k-passthrough',
                                               'STATE='+shlex.quote(str(self.root/'state')))
        text = text.replace('/tmp/dnsmasq.', str(self.root/'dnsmasq.'))
        text = text.replace('/etc/init.d/dnsmasq', str(self.root/'bin/dnsmasq-control'))
        text = text.replace('/sys/class/net/pon/address', str(self.root/'pon-mac'))
        self.write('pon-mac', '02:aa:bb:cc:dd:ee\n')
        self.source = self.write('functions.sh', text)
        self.lease()
        cmd('ip', 'link', 'add', 'pon', 'type', 'dummy')
        cmd('ip', 'link', 'set', 'pon', 'address', '02:aa:bb:cc:dd:ee', 'addrgenmode', 'none', 'up')
        cmd('ip', 'link', 'add', 'br-lan', 'type', 'bridge')
        cmd('ip', 'link', 'set', 'br-lan', 'address', '02:aa:bb:cc:dd:01', 'addrgenmode', 'none', 'up')
        cmd('ip', '-6', 'address', 'add', 'fe80::88/64', 'dev', 'pon', 'nodad')
        cmd('ip', '-6', 'address', 'add', 'fe80::99/64', 'dev', 'br-lan', 'nodad')
        cmd('ip', 'address', 'add', '198.51.100.10/24', 'dev', 'pon')
        cmd('ip', 'address', 'add', '192.168.0.1/24', 'dev', 'br-lan')
        # Sentinel represents the independent native PD route and must survive.
        cmd('ip', '-6', 'route', 'add', '2001:db8:1:8::/61', 'dev', 'br-lan')
        Path('/proc/sys/net/ipv4/ip_forward').write_text('1\n')
        Path('/proc/sys/net/ipv4/conf/pon/rp_filter').write_text('1\n')
        self.ipv6 = cmd('ip', '-6', 'route', 'show', 'table', 'all').replace(' linkdown', '')
        fw4 = 'table inet fw4 {\n'+(PKG/'fw4-set.nft').read_text()+'''
chain srcnat_wan {\n'''+(PKG/'fw4-nat.nft').read_text()+'''}
chain forward_wan {\n'''+(PKG/'fw4-forward.nft').read_text()+'''}
}
'''
        cmd('nft', '-f', '-', input=fw4)

    def tearDown(self):
        cmd('nft', 'flush', 'ruleset')
        # The namespace is a disposable test fixture; these are its rules only.
        cmd('ip', '-4', 'rule', 'flush')
        if not cmd('ip', '-4', 'rule', 'show').startswith('0:'):
            cmd('ip', '-4', 'rule', 'add', 'pref', '0', 'lookup', 'local')
        cmd('ip', '-4', 'rule', 'add', 'pref', '32766', 'lookup', 'main')
        cmd('ip', '-4', 'rule', 'add', 'pref', '32767', 'lookup', 'default')
        cmd('ip', 'link', 'del', 'pon')
        cmd('ip', 'link', 'del', 'br-lan')

    def write(self, name, text, executable=False):
        p = self.root/name
        p.write_text(text)
        if executable: p.chmod(0o755)
        return p

    def lease(self, name='wan.json', address='198.51.100.10', dns=None, gateway='198.51.100.1', up=True):
        return self.write(name, json.dumps({'up': up, 'l3_device': 'pon',
            'ipv4-address': [{'address': address, 'mask': 24}],
            'route': [{'target': '203.0.113.0', 'mask': 24, 'nexthop': '198.51.100.254'},
                      {'target': '0.0.0.0', 'mask': 0, 'nexthop': gateway}],
            'dns-server': dns or ['203.0.113.53', '2001:db8::53']}))

    def run_shell(self, body, expected=0):
        script = f'''set -eu
set -- {MAC}
export Q1000K_PT_FUNCTIONS_ONLY=1
ip() {{ /usr/sbin/ip "$@"; }}
. {shlex.quote(str(self.source))}
preflight
trap cleanup EXIT
{body}
'''
        p = subprocess.run(['busybox', 'ash', '-c', script], env=self.env,
                           text=True, capture_output=True, timeout=30)
        self.assertEqual(p.returncode, expected, p.stdout+p.stderr)
        # The wire fixture temporarily adds/removes the bridge's only port.
        # Carrier flags can change; IPv6 route destinations/next hops cannot.
        self.assertEqual(cmd('ip', '-6', 'route', 'show', 'table', 'all').replace(' linkdown', ''), self.ipv6)
        self.assertEqual(len(cmd('ip', '-4', 'rule', 'show').splitlines()), 3)
        self.assertTrue(cmd('ip', '-4', 'rule', 'show').startswith('0:'))
        self.assertEqual(Path('/proc/sys/net/ipv4/conf/pon/rp_filter').read_text().strip(), '1')
        return p

    def test_acquire_dns_gateway_renumber_loss_reacquire_and_cleanup(self):
        self.lease('dns.json', dns=['203.0.113.54'])
        self.lease('gateway.json', gateway='198.51.100.254', dns=['203.0.113.54'])
        self.lease('renumber.json', address='198.51.100.20')
        self.lease('down.json', up=False)
        self.run_shell('''
block_dhcp
reconcile
grep -q 'option:router,198.51.100.1' "$fragment"
ip -4 route show table 100 | grep -q '198.51.100.10 dev br-lan'
nft list set inet fw4 q1000k_pt4 | grep -q '198.51.100.10'
test "$(wc -l < "$PT_TEST_ROOT/restarts")" = 2
reconcile
test "$(wc -l < "$PT_TEST_ROOT/restarts")" = 2
cp "$PT_TEST_ROOT/dns.json" "$PT_TEST_ROOT/wan.json"
reconcile
grep -q 'option:dns-server,203.0.113.54' "$fragment"
test "$(wc -l < "$PT_TEST_ROOT/restarts")" = 3
cp "$PT_TEST_ROOT/gateway.json" "$PT_TEST_ROOT/wan.json"
reconcile
grep -q 'option:router,198.51.100.254' "$fragment"
cp "$PT_TEST_ROOT/renumber.json" "$PT_TEST_ROOT/wan.json"
reconcile
test "$(ip -4 route show table 100 | wc -l)" = 1
ip -4 route show table 100 | grep -q '198.51.100.20 dev br-lan'
! ip neigh show dev br-lan | grep -q 198.51.100.10
grep -q -- '-s 198.51.100.10' "$PT_TEST_ROOT/conntrack.calls"
cp "$PT_TEST_ROOT/down.json" "$PT_TEST_ROOT/wan.json"
reconcile
test -z "$(ip -4 route show table 100)"
grep -q 'dhcp-host=02:11:22:33:44:55,ignore' "$fragment"
test ! -f "$STATE/lease"
cp "$PT_TEST_ROOT/renumber.json" "$PT_TEST_ROOT/wan.json"
reconcile
ip -4 route show table 100 | grep -q '198.51.100.20 dev br-lan'
''')
        self.assertFalse((self.root/'dnsmasq.test.d/q1000k-passthrough.conf').exists())
        self.assertNotIn('198.51.100.', cmd('nft', 'list', 'set', 'inet', 'fw4', 'q1000k_pt4'))

    def test_firewall_reload_repopulates_set_without_dhcp_restart(self):
        self.run_shell('''
block_dhcp
reconcile
nft flush set inet fw4 q1000k_pt4
reconcile
nft list set inet fw4 q1000k_pt4 | grep -q '198.51.100.10'
test "$(wc -l < "$PT_TEST_ROOT/restarts")" = 2
''')

    def test_missing_generated_dhcp_fragment_is_recreated(self):
        self.run_shell('''
block_dhcp
reconcile
rm "$fragment"
reconcile
grep -q 'dhcp-host=02:11:22:33:44:55,set:q1000k_pt,198.51.100.10' "$fragment"
test "$(wc -l < "$PT_TEST_ROOT/restarts")" = 3
''')

    def test_malformed_or_missing_wan_state_withdraws_public_service(self):
        self.write('bad.json', '{')
        self.run_shell('''
block_dhcp
reconcile
cp "$PT_TEST_ROOT/bad.json" "$PT_TEST_ROOT/wan.json"
reconcile
test -z "$(ip -4 route show table 100)"
grep -q ',ignore' "$fragment"
''')

    def test_foreign_fragment_is_retained_but_routing_is_restored(self):
        self.run_shell('''
block_dhcp
reconcile
printf '# user change\\n' >> "$fragment"
''', expected=1)
        self.assertIn('# user change', (self.root/'dnsmasq.test.d/q1000k-passthrough.conf').read_text())

    def test_missing_pon_device_aborts_and_restores_routing(self):
        self.run_shell('''
block_dhcp
reconcile
rm "$PT_TEST_ROOT/pon-mac"
reconcile
''', expected=1)

    def test_dnsmasq_restart_failure_restores_routes_and_retains_failure_state(self):
        self.lease('dns.json', dns=['203.0.113.54'])
        self.run_shell('''
block_dhcp
reconcile
cp "$PT_TEST_ROOT/dns.json" "$PT_TEST_ROOT/wan.json"
touch "$PT_TEST_ROOT/fail-restart"
reconcile
''', expected=1)
        self.assertTrue((self.root/'state').exists())
        self.assertEqual(cmd('ip', '-4', 'route', 'show', 'table', '100'), '')

    @unittest.skipUnless(os.environ.get('PT_HOST_TOOLS') and os.environ.get('PT_WIRE_OUTPUT'),
                         'Build host tools and select an evidence output directory for real DHCP tests')
    def test_dual_stack_native_dhcp_exchanges(self):
        wire = ROOT/'tests/q1000k/passthrough_wire.py'
        self.run_shell(f'''
block_dhcp
reconcile
python3 {shlex.quote(str(wire))} --tools {shlex.quote(os.environ['PT_HOST_TOOLS'])} \\
 --root {shlex.quote(os.environ['PT_WIRE_OUTPUT'])} --fragment "$fragment"
''')


if __name__ == '__main__':
    unittest.main(verbosity=2)
