#!/usr/bin/env python3
"""Failure-aware cleanup of the installed-service bench; no host networking."""
import os,re,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
class RecoveryTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory(prefix='q1000k-integrated-bench-');self.addCleanup(self.temp.cleanup)
  self.root=Path(self.temp.name);self.state=self.root/'var/run/q1000k-l3-bench';self.state.mkdir(parents=True)
  self.env=dict(os.environ,TEST_ROOT=str(self.root),PATH=str(self.root/'bin')+':'+os.environ['PATH'])
  self.original={}
  for cfg in ['network','dhcp','firewall','xgspon']:
   self.original[cfg]='original '+cfg+'\n';self.write('etc/config/'+cfg,'changed\n');self.write('var/run/q1000k-l3-bench/'+cfg,self.original[cfg])
  for name,value in {'saved':'','active':'','token':'fixture','deadline':'0','pon-mac':'02:00:00:00:00:01','management-neighbor':'192.0.2.2 02:00:00:00:00:42','rules-before':'0: from all lookup local\n'}.items():self.write('var/run/q1000k-l3-bench/'+name,value+'\n')
  self.write('sys/class/net/pon/address','02:00:00:00:00:01\n');self.write('proc/uptime','10.00 0.00\n')
  self.write('sys/firmware/devicetree/base/quantum,xgspon-activation-bench','');self.write('etc/q1000k-private-autostart','')
  (self.root/'tmp').mkdir()
  stub='''#!/bin/sh
printf '%s %s\\n' "${0##*/}" "$*" >> "$TEST_ROOT/calls"
case "${0##*/}:$*" in
 network:reload) [ "$FAIL_NETWORK" != 1 ] || exit 1;;
 q1000k-ipv6-transition:*) [ "$FAIL_IPV6" != 1 ] || exit 1;;
 ip:'-4 rule show') cat "$TEST_ROOT/var/run/q1000k-l3-bench/rules-before";;
esac
exit 0
'''
  for name in ['ip','uci','nft']:self.write('bin/'+name,stub,True)
  for name in ['network','firewall','dnsmasq','odhcpd','q1000k-passthrough']:self.write('etc/init.d/'+name,stub,True)
  self.write('usr/libexec/q1000k-ipv6-transition',stub,True)
  self.write('var/run/q1000k-l3-bench/ipv6-transition/known','fixture\n')
  self.write('var/run/q1000k-l3-bench/ipv6-transition/snapshot.ready','')
  for src,dest in [('ip-passthrough','q1000k-ip-passthrough'),('l3-bench','q1000k-l3-bench')]:
   s=(ROOT/'package/network/utils/q1000k-xgspon-bench/files'/src).read_text()
   s=re.sub(r'/(?:var|sys|etc|proc|usr|tmp)/',lambda m:str(self.root)+m.group(),s)
   s=re.sub(r'\bip (?=(?:-4|neigh|route|address|link)\b)',str(self.root/'bin/ip')+' ',s)
   self.write('usr/sbin/'+dest,s,True)
 def write(self,path,data,exe=False):
  p=self.root/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(data)
  if exe:p.chmod(0o755)
  return p
 def run_helper(self,*args,**env):return subprocess.run(['busybox','ash',str(self.root/'usr/sbin/q1000k-l3-bench'),*args],env=self.env|env,capture_output=True,text=True,timeout=8)
 def test_restores_four_configs_without_restarting_optics_and_keeps_management(self):
  p=self.run_helper('stop','fixture');self.assertEqual(p.returncode,0,p.stderr)
  for cfg,value in self.original.items():self.assertEqual((self.root/'etc/config'/cfg).read_text(),value)
  calls=(self.root/'calls').read_text();self.assertIn('q1000k-passthrough stop',calls);self.assertNotIn('q1000k-xgspon',calls)
  self.assertLess(calls.index('network reload'),calls.index('q1000k-ipv6-transition apply'))
  self.assertLess(calls.index('q1000k-ipv6-transition apply'),calls.index('odhcpd restart'))
  self.assertIn('ip neigh replace',calls);self.assertFalse(self.state.exists())
  self.assertEqual(self.run_helper('management-release','fixture').returncode,0)
 def test_failure_retains_guard_and_configs_then_retry_completes(self):
  self.assertNotEqual(self.run_helper('stop','fixture',FAIL_NETWORK='1').returncode,0)
  self.assertTrue((self.state/'active').exists());self.assertTrue((self.state/'xgspon').exists())
  self.assertEqual(self.run_helper('stop','fixture').returncode,0)
 def test_wrong_token_cannot_restore(self):
  self.assertEqual(self.run_helper('stop','wrong').returncode,0)
  self.assertTrue((self.state/'active').exists());self.assertFalse((self.root/'calls').exists())
 def test_ipv6_repair_failure_retains_guard_for_retry(self):
  self.assertNotEqual(self.run_helper('stop','fixture',FAIL_IPV6='1').returncode,0)
  self.assertTrue((self.state/'active').exists())
  self.assertEqual(self.run_helper('stop','fixture').returncode,0)
 def test_deadline_restores_without_host_heartbeat(self):
  p=self.run_helper('guard','fixture');self.assertEqual(p.returncode,0,p.stderr);self.assertFalse(self.state.exists())
if __name__=='__main__':unittest.main()
