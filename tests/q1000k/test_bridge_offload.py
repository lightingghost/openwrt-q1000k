#!/usr/bin/env python3
"""Exercise the production bridge rule service against isolated UCI/nft fixtures."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
PACKAGE = REPO / 'package/network/config/bridge-hw-offload/files'
SCRIPT = PACKAGE / 'usr/share/bridge-hw-offload/apply-rules.sh'

class BridgeServiceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='q1000k-bridge-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.rules = self.root / 'var/run/bridge-hw-offload/bridge.nft'
        self.brif = self.root / 'sys/class/net/br-lan/brif'
        for port in ('lan1', 'lan2'):
            (self.brif / port).mkdir(parents=True)
        self.env = dict(os.environ, PATH=str(self.bin)+':'+os.environ['PATH'], ROOT=str(self.root), SW='1', HW='1')
        self.write_tool('uci', '''#!/bin/sh
case "$*" in *flow_offloading_hw) echo "$HW";; *flow_offloading) echo "$SW";; esac
''')
        self.write_tool('logger', '#!/bin/sh\nprintf "%s\\n" "$*" >> "$ROOT/log"\n')
        self.write_tool('nft', '''#!/bin/sh
[ "$1" != destroy ] || exit 0
cp "$3" "$ROOT/checked.nft"
[ "$FAIL_CHECK" != 1 ]
''')
        self.write_tool('firewall', '''#!/bin/sh
printf '%s\n' "$*" >> "$ROOT/reloads"
[ "$FAIL_RELOAD" != 1 ]
''')
        source=SCRIPT.read_text()
        for path in ['/var/run/bridge-hw-offload', '/var/lock', '/sys/class/net/']:
            source=source.replace(path,str(self.root)+path)
        source=source.replace('/etc/init.d/firewall',str(self.bin/'firewall'))
        self.script=self.root/'apply.sh';self.script.write_text(source)

    def write_tool(self,name,source):
        p=self.bin/name;p.write_text(source);p.chmod(0o755)

    def run_service(self,*args,expected=0,**env):
        p=subprocess.run(['/usr/bin/busybox','ash',str(self.script),*args],env=self.env|env,capture_output=True,text=True)
        self.assertEqual(p.returncode,expected,p.stderr)
        return p

    def test_enable_both_ports_with_established_ip_flows(self):
        self.run_service()
        text=self.rules.read_text()
        self.assertIn('table bridge fw4',text)
        self.assertIn('devices = { "lan1", "lan2" }',text)
        self.assertIn('counter; flags offload;',text)
        self.assertIn('iifname { "lan1", "lan2" } oifname { "lan1", "lan2" } ct state established',text)
        self.assertIn('meta l4proto { tcp, udp } counter flow offload @br_offload',text)
        self.assertTrue((self.root/'checked.nft').read_text().startswith('destroy table bridge fw4\n'))
        self.assertEqual((self.root/'reloads').read_text(),'reload\n')
        self.assertEqual(list(self.rules.parent.glob('.*')),[])

    def test_each_disabled_flag_removes_rules(self):
        for flags in [dict(HW='0'),dict(SW='0'),dict(HW='',SW='')]:
            self.run_service()
            self.run_service(**flags)
            self.assertEqual(self.rules.read_text(),'')
            self.assertEqual((self.root/'checked.nft').read_text(),'destroy table bridge fw4\n')

    def test_early_boot_and_removed_port(self):
        self.run_service()
        (self.brif/'lan2').rmdir()
        self.run_service()
        self.assertEqual(self.rules.read_text(),'')
        (self.brif/'lan1').rmdir()
        self.brif.rmdir()
        self.run_service()
        self.assertEqual(self.rules.read_text(),'')

    def test_network_membership_changes(self):
        (self.brif/'lan2').rmdir()
        (self.brif/'lan3.20').mkdir()
        self.run_service()
        text=self.rules.read_text()
        self.assertNotIn('lan2',text)
        self.assertIn('"lan3.20"',text)

    def test_kernel_validation_failure_preserves_fragment(self):
        self.run_service()
        original=self.rules.read_bytes()
        (self.root/'reloads').unlink()
        self.run_service(expected=1,FAIL_CHECK='1',HW='0')
        self.assertEqual(self.rules.read_bytes(),original)
        self.assertFalse((self.root/'reloads').exists())

    def test_reload_failure_rolls_back(self):
        self.run_service()
        original=self.rules.read_bytes()
        self.run_service(expected=1,FAIL_RELOAD='1',HW='0')
        self.assertEqual(self.rules.read_bytes(),original)

    def test_first_reload_failure_leaves_no_fragment(self):
        self.run_service(expected=1,FAIL_RELOAD='1')
        self.assertFalse(self.rules.exists())

    def test_invalid_names_cannot_inject_nft_commands(self):
        self.run_service('br-lan;flush ruleset',expected=1)
        (self.brif/'bad"name').mkdir()
        self.run_service(expected=1)
        self.assertFalse(self.rules.exists())

    def test_restart_releases_stop_lock_before_start(self):
        self.run_service()
        source=(PACKAGE/'etc/init.d/bridge-hw-offload').read_text()
        for path in ['/var/run/bridge-hw-offload', '/var/lock']:
            source=source.replace(path,str(self.root)+path)
        source=source.replace('/usr/share/bridge-hw-offload/apply-rules.sh',str(self.script))
        source+="\nprocd_open_instance() { :; }\nprocd_close_instance() { :; }\nprocd_set_param() { :; }\nstop_service\nstart_service\n"
        self.script.chmod(0o755)
        init=self.root/'init.sh';init.write_text(source)
        result=subprocess.run(['/usr/bin/busybox','ash',str(init)],env=self.env,capture_output=True,text=True,timeout=3)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('table bridge fw4',self.rules.read_text())

    def test_packaged_include_flushes_stale_table_and_allows_early_boot(self):
        text=(PACKAGE/'usr/share/nftables.d/ruleset-post/30-bridge-offload.nft').read_text()
        self.assertIn('destroy table bridge fw4\ninclude "/var/run/bridge-hw-offload/*.nft"',text)
        init=(PACKAGE/'etc/init.d/bridge-hw-offload').read_text()
        self.assertIn('procd_add_reload_trigger firewall network',init)
        self.assertIn('procd_open_instance',init)
        self.assertNotIn('procd_openinstance',init)

if __name__=='__main__': unittest.main()
