#!/usr/bin/env python3
"""Real UCI commit/staging boundaries with a fake optical lifecycle only."""
import os
import subprocess
import time
import unittest

import test_pon_config
import test_pon_wan

ROOT = test_pon_config.ROOT


class ConfigWatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_pon_wan.WanTests.setUpClass.__func__(cls)

    def setUp(self):
        test_pon_config.ConfigTests.setUp(self)
        self.uci_cmd('set', 'xgspon.service.enabled=1')
        self.uci_cmd('set', 'xgspon.identity.registration_id=0123')
        self.uci_cmd('set', 'xgspon.identity.equipment_id=initial')
        self.commit()
        fake = '''#!/bin/sh
. "{common}"
value=$(pon_config_get identity.equipment_id)
printf 'start:%s:%s\n' "$value" "$1" >> "{root}/events"
trap 'printf "stop:%s\\n" "$value" >> "{root}/events"; /bin/sleep 0.08; printf "stopped:%s\\n" "$value" >> "{root}/events"; [ ! -f "{root}/fail-stop" ] || exit 1; exit 0' TERM INT
while :; do
    [ ! -f "{root}/fail-child" ] || exit 1
    if [ -f "{root}/read-child" ]; then
        pon_config_get identity.equipment_id > "{root}/child-value"
        rm "{root}/read-child"
    fi
    /bin/sleep 0.02 & wait $!
done
'''.format(common=self.backend.common, root=self.root)
        self.fake = self.backend.write('fake-optics', fake)
        self.fake.chmod(0o755)
        source = (ROOT / 'package/network/utils/q1000k-xgspon-service/files/watch-config').read_text()
        source = source.replace('/lib/q1000k-xgspon/common.sh', str(self.backend.common))
        source = source.replace('/etc/config/xgspon', str(self.root/'config/xgspon'))
        source = source.replace('/var/run/q1000k-xgspon-config', str(self.root/'watch-state'))
        source = source.replace('/usr/libexec/q1000k-xgspon-run', str(self.fake))
        source = source.replace('sleep 2 &', '/bin/sleep 0.08 &')
        self.watch = self.backend.write('watch', source)
        self.processes = []
        self.addCleanup(self.stop_all)

    def uci_cmd(self, *args):
        return subprocess.run([str(self.root/'uci'), *args], env=self.env,
                              text=True, capture_output=True, check=True).stdout.strip()

    def commit(self):
        self.uci_cmd('commit', 'xgspon')

    def events(self):
        path = self.root/'events'
        return path.read_text().splitlines() if path.exists() else []

    def await_event(self, value):
        deadline = time.monotonic()+8
        while time.monotonic() < deadline:
            if value in self.events():
                return
            time.sleep(0.02)
        self.fail(f'missing {value}: {self.events()}')

    def launch(self):
        p = subprocess.Popen(['busybox', 'ash', str(self.watch)], env=self.env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.processes.append(p)
        return p

    def stop_all(self):
        for p in self.processes:
            if p.poll() is None:
                p.terminate()
            p.communicate(timeout=8)

    def test_commit_restarts_once_after_complete_shutdown_staging_and_noop_do_not(self):
        self.launch()
        self.await_event('start:initial:')
        self.uci_cmd('set', 'xgspon.identity.equipment_id=next')
        time.sleep(0.4)
        self.assertEqual(self.events(), ['start:initial:'])
        self.commit()
        self.await_event('start:next:')
        self.assertEqual(self.events(), ['start:initial:', 'stop:initial',
                                        'stopped:initial', 'start:next:'])
        self.commit()
        time.sleep(0.4)
        self.assertEqual(len(self.events()), 4)

    def test_snapshot_ignores_edits_staged_after_commit(self):
        self.uci_cmd('set', 'xgspon.identity.equipment_id=uncommitted')
        self.launch()
        self.await_event('start:initial:')
        (self.root/'read-child').touch()
        deadline=time.monotonic()+5
        while (self.root/'read-child').exists() and time.monotonic()<deadline:
            time.sleep(0.02)
        self.assertEqual((self.root/'child-value').read_text().strip(), 'initial')

    def test_passthrough_commits_do_not_restart_optics(self):
        self.launch()
        self.await_event('start:initial:')
        self.uci_cmd('set', 'xgspon.passthrough.mode=l3')
        self.uci_cmd('set', 'xgspon.passthrough.client_mac=02:11:22:33:44:55')
        self.commit()
        time.sleep(0.5)
        self.assertEqual(self.events(), ['start:initial:'])
        self.uci_cmd('set', 'xgspon.identity.equipment_id=changed')
        self.commit()
        self.await_event('start:changed:')

    def test_luci_empty_field_removal_does_not_restart_optics(self):
        config = self.root/'config/xgspon'
        saved = config.read_text()
        self.assertIn("config identity 'identity'", saved)
        config.write_text(saved.replace("config identity 'identity'",
            "config identity 'identity'\n\toption serial ''\n\toption wan_mac ''"))
        self.launch()
        self.await_event('start:initial:')
        config.write_text(saved)
        self.commit()
        time.sleep(0.5)
        self.assertEqual(self.events(), ['start:initial:'])

    def test_invalid_committed_identity_keeps_working_stack_then_accepts_fix(self):
        self.launch()
        self.await_event('start:initial:')
        self.uci_cmd('set', 'xgspon.identity.registration_id=not-hex')
        self.commit()
        time.sleep(0.5)
        self.assertEqual(self.events(), ['start:initial:'])
        self.uci_cmd('set', 'xgspon.identity.registration_id=5678')
        self.uci_cmd('set', 'xgspon.identity.equipment_id=fixed')
        self.commit()
        self.await_event('start:fixed:')

    def test_disabled_service_watches_without_transmitting_and_enable_commit_starts(self):
        self.uci_cmd('set', 'xgspon.service.enabled=0')
        self.uci_cmd('set', 'xgspon.service.monitor=0')
        self.commit()
        self.launch()
        time.sleep(0.4)
        self.assertEqual(self.events(), [])
        self.uci_cmd('set', 'xgspon.service.enabled=1')
        self.commit()
        self.await_event('start:initial:')
        self.uci_cmd('set', 'xgspon.service.enabled=0')
        self.uci_cmd('set', 'xgspon.service.monitor=1')
        self.commit()
        self.await_event('start:initial:--monitor')

    def test_invalid_service_configuration_keeps_running_stack(self):
        self.launch()
        self.await_event('start:initial:')
        self.uci_cmd('set', 'xgspon.service.lower=invalid/name')
        self.commit()
        time.sleep(0.4)
        self.assertEqual(self.events(), ['start:initial:'])

    def test_fault_is_not_retried_with_unchanged_configuration(self):
        self.launch()
        self.await_event('start:initial:')
        (self.root/'fail-child').touch()
        time.sleep(0.6)
        self.commit()
        time.sleep(0.4)
        self.assertEqual(self.events(), ['start:initial:'])

    def test_missing_or_malformed_committed_file_does_not_stop_optics(self):
        self.launch()
        self.await_event('start:initial:')
        config=self.root/'config/xgspon'
        config.unlink()
        time.sleep(0.3)
        config.write_text("config 'unterminated\n")
        time.sleep(0.3)
        self.assertEqual(self.events(), ['start:initial:'])

    def test_wrapper_stop_waits_for_child_and_releases_lock(self):
        p=self.launch()
        self.await_event('start:initial:')
        p.terminate()
        _, stderr=p.communicate(timeout=8)
        self.assertEqual(p.returncode, 0, stderr)
        self.assertEqual(self.events()[-2:], ['stop:initial', 'stopped:initial'])
        self.assertFalse((self.root/'watch-state/lock').exists())

    def test_failed_shutdown_does_not_start_replacement(self):
        self.launch()
        self.await_event('start:initial:')
        (self.root/'fail-stop').touch()
        self.uci_cmd('set', 'xgspon.identity.equipment_id=next')
        self.commit()
        self.await_event('stopped:initial')
        time.sleep(0.4)
        self.assertEqual(self.events(), ['start:initial:', 'stop:initial', 'stopped:initial'])

    def test_invalid_serial_or_mac_is_rejected_before_shutdown(self):
        self.launch()
        self.await_event('start:initial:')
        for key, value in [('serial', 'invalid'), ('wan_mac', 'ff:ff:ff:ff:ff:ff')]:
            self.uci_cmd('set', f'xgspon.identity.{key}={value}')
            self.commit()
            time.sleep(0.4)
            self.assertEqual(self.events(), ['start:initial:'])
            self.uci_cmd('set', f'xgspon.identity.{key}=')

    def test_common_config_reader_supports_nounset_callers(self):
        run = subprocess.run(['busybox', 'ash', '-uc',
            f'. "{self.backend.common}"; pon_config_get identity.equipment_id'],
            env=self.env, text=True, capture_output=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout.strip(), 'initial')


if __name__ == '__main__':
    unittest.main()
