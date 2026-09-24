#!/usr/bin/env python3
"""Exercise the WAN defaults with real UCI in private configuration directories."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'package/network/utils/q1000k-xgspon-wan/files/defaults'


class WanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='q1000k-uci-test-')
        cls.addClassCleanup(cls.build.cleanup)
        build = Path(cls.build.name)
        sources = list((ROOT / 'build_dir/target-aarch64_cortex-a53_musl').glob('uci-*/cli.c'))
        if len(sources) != 1:
            raise RuntimeError('Prepare the target UCI package before running WAN tests')
        source = build / 'source'
        shutil.copytree(sources[0].parent, source)
        header = source / 'uci.h'
        text = header.read_text()
        # Isolate all compiled defaults too: the library adds a default delta
        # search path before processing command-line overrides.
        for old, name in [('/etc/config', 'unused-config'), ('/tmp/.uci', 'unused-delta'),
                          ('/var/run/uci', 'unused-overrides')]:
            text = text.replace('"' + old + '"', '"' + str(build / name) + '"')
        header.write_text(text)
        cls.uci = build / 'uci'
        # The standalone parser/CLI does not use the optional ubus blob adapter.
        files = ['cli.c', 'libuci.c', 'file.c', 'util.c', 'delta.c', 'parse.c']
        subprocess.run(['gcc', '-O2', '-I' + str(source), *[str(source / f) for f in files],
                        '-ldl', '-o', str(cls.uci)], check=True, capture_output=True, text=True)

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='q1000k-wan-config-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        for directory in ['config', 'delta', 'overrides', 'bin']:
            (self.root / directory).mkdir()
        (self.root / 'config/network').write_text("""config device 'br_lan'
 option name 'br-lan'
 option type 'bridge'
 list ports 'lan1'
 list ports 'lan2'
config interface 'lan'
 option device 'br-lan'
 option proto 'static'
 option ipaddr '192.168.1.1'
config interface 'uplink'
 option device 'eth1'
 option proto 'dhcp'
""")
        shutil.copyfile(ROOT / 'package/network/config/firewall/files/firewall.config',
                        self.root / 'config/firewall')
        wrapper = self.root / 'bin/uci'
        wrapper.write_text('#!' + sys.executable + '\n' + '''import os,sys
from pathlib import Path
root=Path(os.environ['WAN_TEST_ROOT'])
if ' '.join(sys.argv[1:]) == os.environ.get('WAN_TEST_FAIL'):
    sys.exit(1)
os.execv(os.environ['WAN_TEST_UCI'], [os.environ['WAN_TEST_UCI'],
    '-c', str(root/'config'), '-C', str(root/'overrides'), '-t', str(root/'delta'), *sys.argv[1:]])
''')
        wrapper.chmod(0o755)
        (self.root / 'bin/sed').symlink_to(shutil.which('sed'))
        # Only UCI and sed are executable through PATH. No networking command,
        # loader, init service or firewall command can reach the host.
        self.env = dict(os.environ, PATH=str(self.root / 'bin'),
                        WAN_TEST_ROOT=str(self.root), WAN_TEST_UCI=str(self.uci))

    def uci_cmd(self, *args):
        return subprocess.run([str(self.root / 'bin/uci'), *args], env=self.env,
                              check=True, text=True, capture_output=True).stdout.strip()

    def install(self, result=0):
        run = subprocess.run(['/bin/sh', str(SCRIPT)], env=self.env, text=True, capture_output=True)
        self.assertEqual(run.returncode, result, run.stderr)
        return run

    def snapshot(self):
        return [(self.root / ('config/' + name)).read_bytes() for name in ['network', 'firewall']]

    def test_normal_image_prefix_defaults_preserve_lan_and_survive_reinstall(self):
        marker = self.root / 'normal-dt'
        marker.touch()
        source = SCRIPT.read_text().replace('/sys/firmware/devicetree/base/quantum,xgspon-service', str(marker))
        run = subprocess.run(['/bin/sh', '-c', source], env=self.env, capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(self.uci_cmd('get', 'network.lan.ipaddr'), '192.168.1.1')
        self.assertEqual(self.uci_cmd('get', 'network.wan6.reqaddress'), 'none')
        self.assertEqual(self.uci_cmd('get', 'network.wan6.ip6hint'), 'f')
        self.assertEqual(self.uci_cmd('get', 'network.lan.ip6assign'), '64')
        self.assertEqual(self.uci_cmd('get', 'network.lan.ip6class'), 'wan6')
        self.assertEqual(self.uci_cmd('get', 'firewall.@defaults[0].flow_offloading'), '1')
        self.assertEqual(self.uci_cmd('get', 'firewall.@defaults[0].flow_offloading_hw'), '1')
        self.uci_cmd('set', 'network.lan.ip6hint=2')
        self.uci_cmd('set', 'firewall.@defaults[0].flow_offloading_hw=0')
        self.uci_cmd('commit', 'firewall')
        self.uci_cmd('commit', 'network')
        before = self.snapshot()
        run = subprocess.run(['/bin/sh', '-c', source], env=self.env, capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_normal_image_preserves_existing_offload_choice_on_first_install(self):
        self.uci_cmd('set', 'firewall.@defaults[0].flow_offloading=0')
        self.uci_cmd('set', 'firewall.@defaults[0].flow_offloading_hw=0')
        self.uci_cmd('commit', 'firewall')
        marker = self.root / 'normal-dt'
        marker.touch()
        source = SCRIPT.read_text().replace('/sys/firmware/devicetree/base/quantum,xgspon-service', str(marker))
        run = subprocess.run(['/bin/sh', '-c', source], env=self.env, capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(self.uci_cmd('get', 'firewall.@defaults[0].flow_offloading'), '0')
        self.assertEqual(self.uci_cmd('get', 'firewall.@defaults[0].flow_offloading_hw'), '0')

    def test_inactive_dhcp_and_ipv6_share_existing_wan_policy(self):
        before_lan = self.uci_cmd('show', 'network.lan')
        before_bridge = self.uci_cmd('show', 'network.br_lan')
        before_wan = self.uci_cmd('show', 'network.uplink')
        self.install()
        for name, proto in [('wan', 'dhcp'), ('wan6', 'dhcpv6')]:
            self.assertEqual(self.uci_cmd('get', f'network.{name}.auto'), '0')
            self.assertEqual(self.uci_cmd('get', f'network.{name}.device'), 'pon')
            self.assertEqual(self.uci_cmd('get', f'network.{name}.proto'), proto)
            self.assertEqual(self.uci_cmd('get', f'network.{name}.q1000k_profile'), '1')
        self.assertEqual(self.uci_cmd('get', 'network.wan6.reqprefix'), 'auto')
        self.assertEqual(self.uci_cmd('get', 'firewall.@zone[1].network').split(),
                         ['wan', 'wan6'])
        self.assertEqual(self.uci_cmd('show', 'network.lan'), before_lan)
        self.assertEqual(self.uci_cmd('show', 'network.br_lan'), before_bridge)
        self.assertEqual(self.uci_cmd('show', 'network.uplink'), before_wan)
        self.assertEqual(self.uci_cmd('get', 'firewall.@zone[1].input'), 'REJECT')
        self.assertEqual(self.uci_cmd('get', 'firewall.@zone[1].masq'), '1')
        self.assertEqual(self.uci_cmd('changes'), '')
        once = self.snapshot()
        self.install()
        self.assertEqual(self.snapshot(), once)

    def test_reinstallation_preserves_user_edits_and_firewall_removal(self):
        self.install()
        self.uci_cmd('set', 'network.wan.auto=1')
        self.uci_cmd('set', 'network.wan.device=pon.200')
        self.uci_cmd('del_list', 'firewall.@zone[1].network=wan')
        self.uci_cmd('commit')
        changed = self.snapshot()
        self.install()
        self.assertEqual(changed, self.snapshot())

    def test_pending_user_changes_are_not_committed(self):
        before = self.snapshot()
        self.uci_cmd('set', 'network.lan.ipaddr=192.168.7.1')
        changes = self.uci_cmd('changes', 'network')
        self.install(1)
        self.assertEqual(before, self.snapshot())
        self.assertEqual(changes, self.uci_cmd('changes', 'network'))

    def test_missing_duplicate_and_foreign_sections_fail_before_edits(self):
        for scenario in ['missing', 'duplicate', 'foreign']:
            with self.subTest(scenario=scenario):
                saved = self.snapshot()
                if scenario == 'missing':
                    self.uci_cmd('set', 'firewall.@zone[1].name=external')
                elif scenario == 'duplicate':
                    self.uci_cmd('set', 'firewall.duplicate=zone')
                    self.uci_cmd('set', 'firewall.duplicate.name=wan')
                else:
                    self.uci_cmd('set', 'network.wan=interface')
                    self.uci_cmd('set', 'network.wan.device=br-lan')
                self.uci_cmd('commit')
                before = self.snapshot()
                self.install(1)
                self.assertEqual(before, self.snapshot())
                self.assertEqual(self.uci_cmd('changes'), '')
                for name, data in zip(['network', 'firewall'], saved):
                    (self.root / ('config/' + name)).write_bytes(data)

    def test_interrupted_commit_keeps_interfaces_inactive_and_retry_explicit(self):
        # Force a real firewall update; stock WAN lists now already use our names.
        self.uci_cmd('del_list', 'firewall.@zone[1].network=wan')
        self.uci_cmd('del_list', 'firewall.@zone[1].network=wan6')
        self.uci_cmd('commit', 'firewall')
        self.env['WAN_TEST_FAIL'] = '-q commit firewall'
        self.install(1)
        for name in ['wan', 'wan6']:
            self.assertEqual(self.uci_cmd('get', f'network.{name}.auto'), '0')
            self.assertEqual(self.uci_cmd('get', f'network.{name}.q1000k_profile'), 'pending')
        del self.env['WAN_TEST_FAIL']
        # Setup does not silently commit saved deltas, even its earlier edits.
        self.install(1)
        self.uci_cmd('revert', 'firewall')
        self.install()
        self.assertEqual(self.uci_cmd('get', 'network.wan.q1000k_profile'), '1')
        self.assertEqual(self.uci_cmd('get', 'firewall.@zone[1].network').split().count('wan'), 1)


if __name__ == '__main__':
    unittest.main()
