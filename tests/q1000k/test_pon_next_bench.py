#!/usr/bin/env python3
"""Next-bench configuration transactions and acceptance evidence boundaries."""
import argparse
import json
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import unittest
import test_pon_wan

ROOT=Path(__file__).resolve().parents[2]
S=importlib.util.spec_from_file_location('next_collector',ROOT/'scripts/q1000k/activation-collect.py')
C=importlib.util.module_from_spec(S); S.loader.exec_module(C)

class NativeConfigTests(unittest.TestCase):
    setUpClass=classmethod(test_pon_wan.WanTests.setUpClass.__func__)
    uci_cmd=test_pon_wan.WanTests.uci_cmd
    def setUp(self):
        test_pon_wan.WanTests.setUp(self)
        self.env['PATH'] += ':' + os.environ['PATH']
        subprocess.run(['/bin/sh',str(test_pon_wan.SCRIPT)],env=self.env,check=True)
        (self.root/'config/dhcp').write_text("config dhcp 'lan'\n option interface 'lan'\n option ra 'disabled'\n option dhcpv6 'disabled'\n option ignore '1'\n")
        (self.root/'lease').mkdir(); (self.root/'activation').touch()
        for cmd in ('ubus','odhcpd','ip'):
            f=self.root/'bin'/cmd; f.write_text('#!/bin/sh\nexit 0\n');f.chmod(0o755)
        src=(ROOT/'package/network/utils/q1000k-xgspon-validation/files/ipv6-bench').read_text()
        src=src.replace('/usr/share/libubox/jshn.sh',str(ROOT/'staging_dir/host/share/libubox/jshn.sh'))
        src=src.replace('/var/run/q1000k-pon-bench.lock',str(self.root/'lease'))
        src=src.replace('/sys/firmware/devicetree/base/quantum,xgspon-activation-bench',str(self.root/'activation'))
        src=src.replace('/etc/init.d/odhcpd',str(self.root/'bin/odhcpd'))
        src=src.replace('ip -6 ',str(self.root/'bin/ip')+' -6 ')
        self.script=self.root/'helper'; self.script.write_text(src)
    def run_mode(self,mode):
        return subprocess.run(['busybox','ash',str(self.script),mode],env=self.env,text=True,capture_output=True)
    def test_native_pd_uses_two_allocations_and_restores_configuration(self):
        before={c:self.uci_cmd('export',c) for c in ('network','dhcp')}
        p=self.run_mode('auto-prepare'); self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertEqual(self.uci_cmd('get','network.q1000k_wan6.reqaddress'),'none')
        self.assertEqual(self.uci_cmd('get','network.lan.ip6class'),'q1000k_wan6')
        self.assertNotEqual(self.uci_cmd('get','network.lan.ip6hint'),self.uci_cmd('get','network.q1000k_wan6.ip6hint'))
        self.assertEqual(self.uci_cmd('get','network.lan.ipaddr'),'192.168.1.1')
        p=self.run_mode('auto-cleanup'); self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertEqual({c:self.uci_cmd('export',c) for c in before},before)
        self.assertFalse((self.root/'lease/auto-ipv6').exists())
    def test_cleanup_preserves_concurrent_configuration_change(self):
        self.assertEqual(self.run_mode('auto-prepare').returncode,0)
        self.uci_cmd('set','network.lan.ipaddr=192.168.10.1'); self.uci_cmd('commit','network')
        self.assertNotEqual(self.run_mode('auto-cleanup').returncode,0)
        self.assertEqual(self.uci_cmd('get','network.lan.ipaddr'),'192.168.10.1')
    def test_existing_lan_allocation_prevents_mutation(self):
        self.uci_cmd('set','network.lan.ip6assign=64'); self.uci_cmd('commit','network')
        before=self.uci_cmd('export','network')
        self.assertNotEqual(self.run_mode('auto-prepare').returncode,0)
        self.assertEqual(self.uci_cmd('export','network'),before)

class PlanTests(unittest.TestCase):
    def args(self,**kw):
        values=dict(suite='next',physical_only=False,skip_physical=False,rx_only=False,identity='private',cases=None)
        values.update(kw); return argparse.Namespace(**values)
    def test_plan_includes_independent_dark_and_one_loaded_service_outage(self):
        cases=C.discovery_plan(self.args())
        self.assertEqual([c['name'] for c in cases if c['name'] in C.PHYSICAL],['activation-omci-wan-dark','activation-omci-wan-fiber'])
        self.assertTrue(any(c['name']=='activation-omci-wan-auto-renew' for c in cases))
        self.assertTrue(any(c['name']=='activation-omci-wan-soak' for c in cases))
    def test_skip_physical_never_schedules_a_fiber_request(self):
        self.assertFalse(any(c['name'] in C.PHYSICAL for c in C.discovery_plan(self.args(skip_physical=True))))
    def test_incompatible_or_unknown_cases_are_rejected(self):
        for options in ({'identity':None},{'rx_only':True},{'cases':'unknown'},{'physical_only':True}):
            with self.assertRaises(ValueError): C.discovery_plan(self.args(**options))

class SourceEvidenceTests(unittest.TestCase):
    def test_success_requires_both_destinations_and_current_delegated_source(self):
        lease=json.dumps({'proto':'dhcpv6','ipv6-prefix':[{'address':'2001:db8:10::','mask':60,'preferred':1000}]})
        text='native_pd_probe phase=initial\n'+lease+'\n'
        for remote in ('2001:4860:4860::8888','2606:4700:4700::1111'):
            text+=f'native_http local=2001:db8:10:f::1 remote={remote} code=200\n'
        self.assertTrue(C.native_pd_summary(text)['all_router_https_verified'])
        self.assertFalse(C.native_pd_summary(text.replace('2001:db8:10:f::1','2001:db8:99::1'))['all_router_https_verified'])
        self.assertFalse(C.native_pd_summary(text.rsplit('native_http',1)[0])['all_router_https_verified'])
        self.assertFalse(C.native_pd_summary(text)['configuration_restored'])
        # A later prefix cannot validate a failed earlier cycle.
        changed=text+'native_pd_probe phase=after-renew\n'+lease.replace('db8:10','db8:20')+'\n'
        changed+='native_http local=2001:db8:10:f::1 remote=2001:4860:4860::8888 code=200\n'
        self.assertFalse(C.native_pd_summary(changed)['all_router_https_verified'])

if __name__=='__main__': unittest.main()
