#!/usr/bin/env python3
"""Validate actual IPv6 source selection and evidence attribution."""
import importlib.util
import ipaddress
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('collector', ROOT/'scripts/q1000k/activation-collect.py')
C = importlib.util.module_from_spec(spec)
spec.loader.exec_module(C)


class SourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='q1000k-pd-source-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.exe = Path(cls.tmp.name)/'pd-source'
        subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-O2',
            str(ROOT/'package/network/utils/q1000k-xgspon-validation/src/pd-source.c'),
            '-o', str(cls.exe)], check=True)

    def run_source(self, *args):
        return subprocess.run([str(self.exe), *args], capture_output=True, text=True)

    def test_every_supported_mask_matches_independent_address_math(self):
        for bits in range(48, 65):
            net = ipaddress.IPv6Network(('2001:db8:1234:5678::', bits), strict=False)
            for prefix in (str(net.network_address), net.network_address.exploded):
                p = self.run_source(prefix, str(bits), 'fe80::1/64', 'fd12::1/64', '2001:db8:ffff::1/128')
                self.assertEqual(p.returncode, 0)
                self.assertEqual(ipaddress.IPv6Address(p.stdout.strip()), net.network_address+1)

    def test_no_borrowing_any_part_of_an_assigned_prefix(self):
        for existing in ('2001:db8:1234:5670::1/128', '2001:db8:1234:567f:abcd::8/64'):
            p = self.run_source('2001:db8:1234:5670::', '60', existing)
            self.assertEqual(p.returncode, 3)
            self.assertEqual(p.stdout, '')
        self.assertEqual(self.run_source('2001:db8:1234:5670::', '60', '2001:db8:1234:5680::1/64').returncode, 0)

    def test_invalid_prefixes_and_inventory_emit_no_candidate(self):
        for args in [('fe80::','64'), ('fd00::','64'), ('::','64'), ('2001:db8::1','64'),
                     ('2001:db8:1234:5671::','60'), ('2001:db8::','65'), ('2001:db8::','47'),
                     ('2001:db8::','-1'), ('2001:db8::','64junk'), ('2001:db8::','99999999999999999999'),
                     ('2001:db8::','64','not-an-address'), ('2001:db8::%pon','64')]:
            with self.subTest(args=args):
                p = self.run_source(*args)
                self.assertEqual(p.returncode, 2)
                self.assertEqual(p.stdout, '')

    def test_source_results_require_cleanup_and_preserve_failure_control(self):
        text = '\n'.join(['source_cycle name=initial',
            'source_result origin=ia_na kind=https rc=28',
            'source_result origin=delegated_prefix kind=ping rc=0',
            'source_result origin=delegated_prefix kind=mtu1500 rc=0',
            'source_result origin=delegated_prefix kind=https rc=0'])
        self.assertFalse(C.ipv6_source_summary(text)['delegated_https'])
        summary = C.ipv6_source_summary(text+'\npd_source_cleanup status=passed\n')
        self.assertTrue(summary['delegated_https'])
        self.assertEqual(summary['cycles']['initial']['ia_na']['https'], 28)
        text += '\npd_source_cleanup status=passed\nsource_cycle name=after-renew\nsource_result origin=delegated_prefix kind=https rc=0\n'
        self.assertFalse(C.ipv6_source_summary(text)['delegated_https'])
        self.assertTrue(C.ipv6_source_summary(text+'pd_source_cleanup status=passed\n')['delegated_https'])

    def test_service_attribution_uses_all_completed_cases_and_both_families(self):
        failed = dict(status='observed', provisioned=True, stages={'cleanup':'passed'}, dhcp_ipv4=True,
                      traffic={'ipv4_https':False})
        success = dict(status='observed', provisioned=True, stages={'cleanup':'passed'}, dhcp_ipv4=True,
                       dhcpv6_address=True, dhcpv6_prefix=True, traffic={'ipv4_https':True, 'ipv6_https':False},
                       ipv6_sources={'delegated_https':True})
        summary = C.service_summary([failed, success])
        self.assertEqual(summary, dict(ipv4=True, ipv6_wan_address=False,
                         ipv6_delegated_prefix=True, dual_stack_same_case=True))
        for change in [dict(status='containment-failure'), dict(provisioned=False), dict(stages={'cleanup':'failed'})]:
            self.assertFalse(any(C.service_summary([dict(success, **change)]).values()))

    def test_wan_plan_keeps_working_policies_and_never_requests_a_fiber_cycle(self):
        args = SimpleNamespace(suite='wan', physical_only=False, skip_physical=True, rx_only=False,
                               identity=Path('/private/identity'), cases=None)
        plan = C.discovery_plan(args)
        self.assertEqual([p['name'] for p in plan], ['rx-startup', 'activation-omci-wan-source',
            'activation-omci-wan-lan', 'activation-omci-wan-renew', 'activation-omci-wan-repeat'])
        for case in plan[1:]:
            self.assertEqual((case['live_add'],case['vlan_untagged'],case['ranging_mode']), (31,1,1))
            self.assertNotIn('physical', case)
        args.rx_only = True
        self.assertEqual(len(C.discovery_plan(args)), 1)


if __name__ == '__main__':
    unittest.main()
