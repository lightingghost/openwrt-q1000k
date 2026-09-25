#!/usr/bin/env python3
"""Committed UCI mode/MAC lifecycle with a fake network owner, no host changes."""
import signal
import subprocess
import time
import unittest
import test_pon_config
import test_pon_config_watch

ROOT=test_pon_config.ROOT

class PassthroughWatchTests(unittest.TestCase):
    setUpClass=classmethod(test_pon_config_watch.ConfigWatchTests.setUpClass.__func__)
    uci_cmd=test_pon_config_watch.ConfigWatchTests.uci_cmd
    commit=test_pon_config_watch.ConfigWatchTests.commit
    events=test_pon_config_watch.ConfigWatchTests.events
    await_event=test_pon_config_watch.ConfigWatchTests.await_event
    launch=test_pon_config_watch.ConfigWatchTests.launch
    stop_all=test_pon_config_watch.ConfigWatchTests.stop_all

    def setUp(self):
        test_pon_config.ConfigTests.setUp(self)
        fake='''#!/bin/sh
printf 'start:%s\\n' "$1" >> "ROOT/events"
trap 'printf "stop\\n" >> "ROOT/events"; /bin/sleep 0.08; [ ! -f "ROOT/fail-stop" ] || exit 1; exit 0' TERM INT
trap 'printf "wake\\n" >> "ROOT/events"' USR1
while :; do
 [ ! -f "ROOT/fail-child" ] || exit 1
 /bin/sleep 0.02 & wait $!
done
'''.replace('ROOT',str(self.root))
        self.fake=self.backend.write('fake-owner',fake); self.fake.chmod(0o755)
        source=(ROOT/'package/network/utils/q1000k-passthrough/files/watch-config').read_text()
        for a,b in [('/lib/q1000k-xgspon/common.sh',self.backend.common),('/etc/config/xgspon',self.root/'config/xgspon'),('/var/run/q1000k-passthrough-config',self.root/'watch-state'),('/usr/libexec/q1000k-passthrough-ipv4',self.fake)]:
            source=source.replace(a,str(b))
        self.watch=self.backend.write('watch',source.replace('sleep 2 &','/bin/sleep 0.08 &'))
        self.processes=[]; self.addCleanup(self.stop_all)

    def enable(self,mac='02:11:22:33:44:55'):
        self.uci_cmd('set','xgspon.passthrough.mode=l3')
        self.uci_cmd('set','xgspon.passthrough.client_mac='+mac)
        self.commit()

    def test_router_default_staging_commit_mac_change_and_disable(self):
        self.launch(); time.sleep(0.25)
        self.assertEqual(self.events(),[])
        self.uci_cmd('set','xgspon.passthrough.mode=l3')
        self.uci_cmd('set','xgspon.passthrough.client_mac=02:11:22:33:44:55')
        time.sleep(0.3); self.assertEqual(self.events(),[])
        self.commit(); self.await_event('start:02:11:22:33:44:55')
        self.enable('02:AA:BB:CC:DD:EE'); self.await_event('start:02:aa:bb:cc:dd:ee')
        self.assertEqual(self.events(),['start:02:11:22:33:44:55','stop','start:02:aa:bb:cc:dd:ee'])
        self.uci_cmd('set','xgspon.passthrough.mode=router'); self.commit()
        time.sleep(0.4); self.assertEqual(self.events()[-1],'stop')

    def test_identity_commit_and_noop_do_not_restart_and_wan_event_wakes(self):
        self.enable(); p=self.launch(); self.await_event('start:02:11:22:33:44:55')
        self.uci_cmd('set','xgspon.identity.equipment_id=changed'); self.commit()
        time.sleep(0.4); self.assertEqual(len(self.events()),1)
        self.commit(); p.send_signal(signal.SIGUSR1); self.await_event('wake')
        self.assertEqual(self.events(),['start:02:11:22:33:44:55','wake'])

    def test_invalid_commits_and_missing_file_retain_current_mode(self):
        self.enable(); self.launch(); self.await_event('start:02:11:22:33:44:55')
        for value in ['ff:ff:ff:ff:ff:ff','','02:11:22:33:44:55\n']:
            self.enable(value); time.sleep(0.35)
            self.assertEqual(len(self.events()),1)
        self.uci_cmd('set','xgspon.passthrough.mode=unknown'); self.commit()
        time.sleep(0.35); (self.root/'config/xgspon').unlink(); time.sleep(0.3)
        self.assertEqual(len(self.events()),1)

    def test_snapshot_ignores_uncommitted_mac(self):
        self.enable(); self.uci_cmd('set','xgspon.passthrough.client_mac=02:aa:bb:cc:dd:ee')
        self.launch(); self.await_event('start:02:11:22:33:44:55')

    def test_fault_not_respawned_by_poll_or_wan_event(self):
        self.enable(); p=self.launch(); self.await_event('start:02:11:22:33:44:55')
        (self.root/'fail-child').touch(); time.sleep(0.4)
        p.send_signal(signal.SIGUSR1); self.commit(); time.sleep(0.4)
        self.assertEqual(self.events(),['start:02:11:22:33:44:55'])

    def test_failed_cleanup_blocks_replacement(self):
        self.enable(); p=self.launch(); self.await_event('start:02:11:22:33:44:55')
        (self.root/'fail-stop').touch(); self.enable('02:aa:bb:cc:dd:ee')
        _,stderr=p.communicate(timeout=8)
        self.assertEqual(p.returncode,1,stderr)
        self.assertEqual(self.events(),['start:02:11:22:33:44:55','stop'])

if __name__=='__main__': unittest.main()
