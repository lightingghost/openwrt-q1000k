#!/usr/bin/env python3
"""Exercise source preference rollback, evidence attribution and client guards."""
import importlib.util
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = ROOT/'package/network/utils/q1000k-xgspon-validation'
SPEC = importlib.util.spec_from_file_location('ipv6_collect', ROOT/'scripts/q1000k/activation-collect.py')
C = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(C)


class PreferenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='q1000k-ipv6-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root/'var/run/q1000k-pon-bench.lock').mkdir(parents=True)
        (self.root/'proc').mkdir(); (self.root/'proc/uptime').write_text('100.0 0.0\n')
        self.env = dict(os.environ, Q6_FIXTURE=str(self.root), PATH=str(self.root)+':'+str(ROOT/'staging_dir/host/bin')+':'+os.environ['PATH'])
        mock = r'''import json,os,sys,pathlib
r=pathlib.Path(os.environ['Q6_FIXTURE']); name=pathlib.Path(sys.argv[0]).name; a=sys.argv[1:]
with (r/'calls').open('a') as f: f.write(json.dumps([name]+a)+'\n')
bad=os.environ.get('Q6_FAIL')
if name=='ip':
 if bad=='link-delete' and a==['link','delete','q6router']: sys.exit(1)
 if a==['-6','-j','address','show','dev','pon']:
  pref=int((r/'preference').read_text()) if (r/'preference').exists() else 3600
  print(json.dumps([{'addr_info':[{'local':'2001:db8::2','prefixlen':128,'preferred_life_time':pref,'valid_life_time':3600}]}]))
 elif a[:3]==['-6','address','change']:
  pref=a[a.index('preferred_lft')+1]
  if bad=='restore' and pref!='0': sys.exit(1)
  assert a[a.index('valid_lft')+1]=='3600'
  (r/'preference').write_text(pref)
 else: print('2606:4700:4700::1111 dev pon src 2001:db8:1::1')
elif name=='uci':
 if a==['-q','changes','dhcp']: print('dhcp.lan.ra=server')
else: pass
'''
        for name in ('ip','nft','curl','ping','uci'):
            p=self.root/name; p.write_text('#!'+sys.executable+'\n'+mock); p.chmod(0o755)
        source=(PACKAGE/'files/ipv6-bench').read_text()
        source=source.replace('/usr/share/libubox/jshn.sh',str(ROOT/'staging_dir/host/share/libubox/jshn.sh'))
        source=source.replace('/usr/bin/ping',str(self.root/'ping'))
        source=source.replace('/var/run/',str(self.root/'var/run')+'/').replace('/proc/uptime',str(self.root/'proc/uptime'))
        # Host BusyBox ash can prefer its built-in ip applet over PATH.
        for name in ('ip','nft','curl','uci'):
            source=re.sub(r'(?<![A-Za-z0-9_/=-])'+name+r'(?= )',str(self.root/name),source)
        self.script=self.root/'experiment'; self.script.write_text(source)

    def run_source(self):
        return subprocess.run(['busybox','ash',str(self.script),'source','2001:db8::2','2001:db8:1::1'],env=self.env,capture_output=True,text=True,timeout=10)

    def test_preference_is_reversible_and_explicit_failure_source_is_retained(self):
        p=self.run_source()
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertEqual((self.root/'preference').read_text(),'3600')
        calls=[json.loads(s) for s in (self.root/'calls').read_text().splitlines()]
        changes=[c for c in calls if c[:4]==['ip','-6','address','change']]
        self.assertEqual([c[c.index('preferred_lft')+1] for c in changes],['0','3600'])
        self.assertTrue(any(c[0]=='ping' and '2001:db8::2' in c and c.count('-I')==2 for c in calls))
        self.assertIn('ipv6_experiment_cleanup mode=source rc=0',p.stdout)
        self.assertFalse(any(c[:4] in (['ip','-6','address','del'],['ip','-6','route','replace']) for c in calls))

    def test_restore_failure_is_not_reported_as_success(self):
        self.env['Q6_FAIL']='restore'
        p=self.run_source()
        self.assertNotEqual(p.returncode,0)
        self.assertIn('ipv6_experiment_cleanup mode=source rc=1',p.stdout)
        self.assertFalse(C.ipv6_experiment_summary(p.stdout)['source_preference_https'])

    def test_uncommitted_settings_prevent_virtual_lan_mutations(self):
        p=subprocess.run(['busybox','ash',str(self.script),'lan'],env=self.env,capture_output=True,text=True,timeout=5)
        self.assertNotEqual(p.returncode,0)
        calls=[json.loads(s) for s in (self.root/'calls').read_text().splitlines()]
        self.assertEqual(calls,[['uci','-q','changes','dhcp']])

    def test_cleanup_reports_failure_to_remove_an_owned_link(self):
        s=self.script.read_text(); s=s[:s.rindex('case "$mode" in')]+'link_client=1\n'
        self.script.write_text(s); self.env['Q6_FAIL']='link-delete'
        p=subprocess.run(['busybox','ash',str(self.script),'lan'],env=self.env,capture_output=True,text=True,timeout=5)
        self.assertNotEqual(p.returncode,0)
        self.assertIn('ipv6_experiment_cleanup mode=lan rc=1',p.stdout)

    def test_unowned_namespace_resolver_survives_cleanup(self):
        resolver=self.root/'etc/netns/q6client/resolv.conf'; resolver.parent.mkdir(parents=True)
        resolver.write_text('existing configuration\n')
        work=self.root/'capture'; work.mkdir()
        s=self.script.read_text().replace('/etc/netns/',str(self.root/'etc/netns')+'/')
        s=s[:s.rindex('case "$mode" in')]+f'work={work}\n'
        self.script.write_text(s)
        p=subprocess.run(['busybox','ash',str(self.script),'physical'],env=self.env,capture_output=True,text=True,timeout=5)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        self.assertEqual(resolver.read_text(),'existing configuration\n')
        self.assertFalse(work.exists())


class EvidenceTests(unittest.TestCase):
    def test_renewal_requires_a_matching_reply_in_the_wan_capture(self):
        sample='ipv6_packets_begin\nDHCPv6, dhcp6 renew (xid=12ac\nDHCPv6, dhcp6 reply (xid=bad1\nipv6_packets_end\n'
        self.assertEqual(C.ipv6_experiment_summary(sample)['renew_reply_xids'],[])
        sample=sample.replace('xid=bad1','xid=12ac')
        self.assertEqual(C.ipv6_experiment_summary(sample)['renew_reply_xids'],['12ac'])
        self.assertFalse(C.ipv6_experiment_summary(sample)['packet_capture_complete'])
        self.assertTrue(C.ipv6_experiment_summary(sample+'ipv6_packet_capture rc=0\n')['packet_capture_complete'])
        self.assertEqual(C.ipv6_experiment_summary('renew_result interface=q1000k_wan6 rc=0\n')['renew_reply_xids'],[])

    def test_firewall_negative_needs_positive_controls(self):
        sample='ipv6_check name=firewall-unsolicited-raw rc=1\n'
        self.assertFalse(C.ipv6_experiment_summary(sample)['unsolicited_udp_blocked'])
        sample+='ipv6_check name=firewall-inbound-listener-control rc=0\nipv6_check name=firewall-outbound-return rc=0\n'
        self.assertTrue(C.ipv6_experiment_summary(sample)['unsolicited_udp_blocked'])

    def test_host_probes_reject_indirect_management_before_any_traffic(self):
        with patch.object(C.subprocess,'run',return_value=subprocess.CompletedProcess([],0,json.dumps([dict(dev='wifi0',gateway='192.168.1.1')]),'')) as run:
            result=C.host_lan_probe('2001:db8:1::1/64')
        self.assertEqual(result['status'],'unavailable')
        self.assertEqual(run.call_count,1)
        self.assertFalse(result['configuration_changed'])

    def test_host_ipv6_probe_does_not_count_management_or_another_source_prefix(self):
        def run(argv,**kwargs):
            if argv[:4]==['ip','-j','route','get']: payload=[dict(dev='lo')]
            elif 'address' in argv: payload=[dict(addr_info=[dict(family='inet6',local='2001:db8:2::2',preferred_life_time=100)])]
            else: payload=[]
            return subprocess.CompletedProcess(argv,0,json.dumps(payload),'')
        with patch.object(C.subprocess,'run',side_effect=run), patch.object(C.time,'monotonic',side_effect=[0,1,46]), patch.object(C.time,'sleep'):
            result=C.host_lan_probe('2001:db8:1::1/64')
        self.assertEqual(result['status'],'unavailable')
        self.assertFalse(result['tests'])


class UdpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='q1000k-udp6-'); cls.addClassCleanup(cls.temp.cleanup)
        cls.exe=Path(cls.temp.name)/'udp6'
        subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror',str(PACKAGE/'src/udp6-probe.c'),'-o',str(cls.exe)],check=True)

    def test_bad_arguments_are_not_negative_firewall_evidence(self):
        for args in [('client','::1','0'),('client','127.0.0.1','47100'),('client','::1','900000000000000000000'),('client','::1','47100junk')]:
            self.assertEqual(subprocess.run([str(self.exe),*args]).returncode,2)

    def test_udp_response_must_match_the_actual_request(self):
        # Loopback UDP requires no hardware or administrative privileges.
        for echo in (True,False):
            with socket.socket(socket.AF_INET6,socket.SOCK_DGRAM) as sock:
                sock.bind(('::1',0)); sock.settimeout(5)
                def reply():
                    data,peer=sock.recvfrom(4096)
                    sock.sendto(data if echo else b'wrong-response',peer)
                thread=threading.Thread(target=reply); thread.start()
                p=subprocess.run([str(self.exe),'client','::1',str(sock.getsockname()[1])],timeout=5)
                thread.join(5)
                self.assertEqual(p.returncode,0 if echo else 1)


if __name__=='__main__': unittest.main()
