#!/usr/bin/env python3
"""Real IPv6 kernel transitions, only in an empty unshare -Urn namespace.

PT_JSONFILTER selects the pinned host jsonfilter. UCI/netifd status are
fixtures; no router, host interface, DHCP server or optical device is touched.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT/'package/network/utils/q1000k-passthrough/files/ipv6-transition'
LAN = '2001:db8:1234:1200::1'
WAN = '2001:db8:1234:120f::1'
POOL = '2001:db8:1234:1200::'
WAN_POOL = '2001:db8:1234:120f::'


def cmd(*args):
    return subprocess.check_output(args, text=True).strip()


@unittest.skipUnless(os.environ.get('PT_JSONFILTER'), 'needs isolated netns and pinned PT_JSONFILTER')
class TransitionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        links = json.loads(cmd('ip', '-j', 'link', 'show'))
        if [d['ifname'] for d in links] != ['lo']:
            raise RuntimeError('Use an empty unshare -Urn namespace')
        mapping = Path('/proc/self/uid_map').read_text().split()
        if len(mapping) != 3 or mapping[-1] != '1':
            raise RuntimeError('Requires isolated single-user mapping')

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='q1000k-ipv6-transition-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state = self.root/'snapshot'
        (self.root/'bin').mkdir()
        (self.root/'bin/jsonfilter').symlink_to(Path(os.environ['PT_JSONFILTER']).resolve(strict=True))
        for name, text in {
            'ifstatus': 'cat "$PT_TEST_ROOT/$1.json"',
            'uci': 'case "$*" in "-q get network.lan.ip6assign") cat "$PT_TEST_ROOT/lan.mask";; "-q get network.wan6.ip6assign") cat "$PT_TEST_ROOT/wan6.mask";; *) exit 1;; esac',
            'sleep': 'exit 0',  # DAD disabled only on disposable fixture devices.
        }.items():
            path = self.root/'bin'/name
            path.write_text('#!/bin/sh\n'+text+'\n'); path.chmod(0o755)
        self.env = dict(os.environ, PT_TEST_ROOT=str(self.root), PATH=str(self.root/'bin')+':'+os.environ['PATH'])
        for dev in ('br-lan', 'pon'):
            cmd('ip', 'link', 'add', dev, 'type', 'dummy')
            Path('/proc/sys/net/ipv6/conf', dev, 'accept_dad').write_text('0\n')
            cmd('ip', 'link', 'set', dev, 'addrgenmode', 'none', 'up')
        cmd('ip', '-6', 'address', 'add', 'fe80::1/64', 'dev', 'br-lan', 'nodad')
        for dev, addr in [('br-lan', LAN), ('pon', WAN)]:
            cmd('ip', '-6', 'address', 'add', addr+'/64', 'dev', dev, 'noprefixroute')
        cmd('ip', '-6', 'route', 'add', POOL+'/64', 'dev', 'br-lan', 'proto', 'static', 'metric', '1024')
        # Native odhcpd's downstream route must never be deleted by the helper.
        cmd('ip', '-6', 'route', 'add', '2001:db8:1234:1208::/61', 'via', 'fe80::4:1', 'dev', 'br-lan', 'proto', 'static', 'metric', '1024')
        self.configure(64, 64)

    def tearDown(self):
        for dev in ('br-lan', 'pon'):
            cmd('ip', 'link', 'del', dev)

    def configure(self, lan, wan, valid=3600):
        for iface, dev, prefix, addr, mask in [('lan', 'br-lan', POOL, LAN, lan), ('wan6', 'pon', WAN_POOL, WAN, wan)]:
            record = {'up': True, 'l3_device': dev, 'ipv6-prefix-assignment': []}
            if mask:
                record['ipv6-prefix-assignment'].append({'address': prefix, 'mask': mask, 'preferred': valid, 'valid': valid, 'local-address': {'address': addr, 'mask': mask}})
            (self.root/(iface+'.json')).write_text(json.dumps(record))
            (self.root/(iface+'.mask')).write_text(str(mask) if mask else '')

    def run_helper(self, action, success=True):
        result = subprocess.run(['busybox', 'ash', str(HELPER), action, str(self.state)], env=self.env, text=True, capture_output=True, timeout=12)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def addresses(self, dev):
        return {(a['local'], a['prefixlen']) for d in json.loads(cmd('ip', '-j', '-6', 'address', 'show', 'dev', dev)) for a in d['addr_info']}

    def simulate_reload(self):
        self.configure(60, None)
        # Reproduce both observations from the Q1000K rather than mocking ip.
        cmd('ip', '-6', 'address', 'replace', LAN+'/60', 'dev', 'br-lan', 'noprefixroute')
        self.assertIn((LAN, 64), self.addresses('br-lan'))
        cmd('ip', '-6', 'address', 'replace', WAN+'/64', 'dev', 'pon', 'preferred_lft', '0', 'valid_lft', '3600')
        route = cmd('ip', '-6', 'route', 'show', 'exact', WAN_POOL+'/64', 'dev', 'pon', 'proto', 'kernel')
        self.assertTrue(route, 'Fixture must reproduce deprecated WAN connected route')

    def assert_delegated_route(self):
        route = json.loads(cmd('ip', '-j', '-6', 'route', 'get', '2001:db8:1234:120f::2'))[0]
        self.assertEqual(route['gateway'], 'fe80::4:1')
        self.assertEqual(route['dev'], 'br-lan')
        self.assertIn('proto static', cmd('ip', '-6', 'route', 'show', 'exact', POOL+'/64'))

    def test_real_kernel_mask_change_and_stale_wan_route_then_rollback(self):
        self.run_helper('snapshot'); self.simulate_reload(); self.run_helper('apply')
        self.assertIn((LAN, 60), self.addresses('br-lan'))
        self.assertNotIn((LAN, 64), self.addresses('br-lan'))
        self.assertNotIn((WAN, 64), self.addresses('pon'))
        self.assert_delegated_route()
        self.configure(64, 64)
        cmd('ip', '-6', 'address', 'replace', LAN+'/64', 'dev', 'br-lan', 'noprefixroute')
        self.assertIn((LAN, 60), self.addresses('br-lan'))
        self.run_helper('apply')
        self.assertIn((LAN, 64), self.addresses('br-lan'))
        self.assertNotIn((LAN, 60), self.addresses('br-lan'))
        self.assertIn((WAN, 64), self.addresses('pon'))
        self.assertFalse(cmd('ip', '-6', 'route', 'show', 'exact', POOL+'/60', 'proto', 'kernel'))

    def test_retry_after_address_deleted_but_kernel_route_left(self):
        self.run_helper('snapshot'); self.simulate_reload()
        cmd('ip', '-6', 'address', 'del', WAN+'/64', 'dev', 'pon')
        cmd('ip', '-6', 'route', 'replace', WAN_POOL+'/64', 'dev', 'pon', 'proto', 'kernel', 'metric', '256')
        self.run_helper('apply'); self.run_helper('apply')
        self.assert_delegated_route()

    def test_foreign_global_address_refuses_before_mutations(self):
        self.run_helper('snapshot'); self.simulate_reload()
        cmd('ip', '-6', 'address', 'add', '2001:db8:1234:120f::99/64', 'dev', 'pon')
        before = {dev: self.addresses(dev) for dev in ('br-lan', 'pon')}
        result = self.run_helper('apply', False)
        self.assertIn('Unowned global address', result.stderr)
        self.assertEqual(before, {dev: self.addresses(dev) for dev in before})

    def test_expired_pool_cannot_be_reintroduced(self):
        self.run_helper('snapshot'); self.configure(60, None, valid=0)
        self.run_helper('apply', False)
        self.assertIn((LAN, 64), self.addresses('br-lan'))
        self.assertIn((WAN, 64), self.addresses('pon'))

    def test_pending_netifd_configuration_refuses_before_mutations(self):
        self.run_helper('snapshot')
        (self.root/'lan.mask').write_text('60')
        self.run_helper('apply', False)
        self.assertIn((LAN, 64), self.addresses('br-lan'))

    def test_noop_preserves_addresses_and_static_routes(self):
        self.run_helper('snapshot')
        before = {dev: self.addresses(dev) for dev in ('br-lan', 'pon')}
        routes = cmd('ip', '-6', 'route', 'show', 'table', 'all')
        self.run_helper('apply')
        self.assertEqual(before, {dev: self.addresses(dev) for dev in before})
        self.assertEqual(routes, cmd('ip', '-6', 'route', 'show', 'table', 'all'))

    def test_existing_snapshot_is_not_overwritten(self):
        self.run_helper('snapshot')
        before = (self.state/'known').read_text()
        self.configure(60, None)
        self.run_helper('snapshot', False)
        self.assertEqual(before, (self.state/'known').read_text())

    def test_snapshot_refuses_foreign_address_before_setup(self):
        cmd('ip', '-6', 'address', 'add', '2001:db8:ffff::1/64', 'dev', 'pon')
        self.run_helper('snapshot', False)
        self.assertFalse((self.state/'snapshot.ready').exists())
        self.run_helper('apply', False)
        self.assertIn(('2001:db8:ffff::1', 64), self.addresses('pon'))

    def test_incomplete_snapshot_cannot_be_applied(self):
        self.run_helper('snapshot')
        (self.state/'snapshot.ready').unlink()
        self.configure(60, None)
        self.run_helper('apply', False)
        self.assertIn((LAN, 64), self.addresses('br-lan'))

    def test_repaired_address_uses_current_not_saved_lifetime(self):
        self.run_helper('snapshot'); self.simulate_reload()
        self.configure(60, None, valid=120)
        self.run_helper('apply')
        addr = next(a for d in json.loads(cmd('ip', '-j', '-6', 'address', 'show', 'dev', 'br-lan')) for a in d['addr_info'] if a['local'] == LAN)
        self.assertGreater(addr['valid_life_time'], 0)
        self.assertLessEqual(addr['valid_life_time'], 120)
        self.assertLessEqual(addr['preferred_life_time'], addr['valid_life_time'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
