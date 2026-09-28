#!/usr/bin/env python3
"""Explicit committed-config applies with real UCI and fake procd services."""
import json
import os
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

import test_pon_config
import test_pon_wan

ROOT = test_pon_config.ROOT
BASE = ROOT / 'package/network/utils/q1000k-xgspon/files'


class ApplyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        test_pon_wan.WanTests.setUpClass.__func__(cls)

    def setUp(self):
        test_pon_config.ConfigTests.setUp(self)
        self.state = self.root / 'state'
        self.state.mkdir()
        self.events = self.root / 'events'
        self.uci_cmd('set', 'xgspon.identity.registration_id=0123')
        self.uci_cmd('set', 'xgspon.identity.equipment_id=initial')
        self.uci_cmd('commit', 'xgspon')

        helper = (BASE / 'apply.sh').read_text().replace(
            '/lib/q1000k-xgspon/common.sh', str(self.backend.common))
        self.helper = self.backend.write('apply.sh', helper)
        for name in ('optical', 'passthrough'):
            script = f'''#!/bin/sh
[ "$1" != status ] || {{ [ -f "{self.state}/{name}.running" ]; exit $?; }}
printf '{name}:%s\\n' "$1" >> "{self.events}"
[ ! -f "{self.state}/fail-{name}" ] || exit 1
case "$1" in
    stop) rm -f "{self.state}/{name}.key" "{self.state}/{name}.running" ;;
    start|reload)
        rm -f "{self.state}/{name}.running"
        cp "${{Q1000K_CONFIG_APPLY_SNAPSHOT%/*}}/{name}.key" \
            "{self.state}/{name}.key"
        if [ ! -f "{self.state}/unhealthy-{name}" ]; then
            touch "{self.state}/{name}.running"
            printf '{{"stage":"running"}}\\n' > "{self.state}/{name}-status.json"
        fi ;;
    *) exit 1 ;;
esac
'''
            self.backend.write(name + '-init', script).chmod(0o755)
        command = (BASE / 'reload-config').read_text()
        for original, replacement in (
            ('/lib/q1000k-xgspon/apply.sh', self.helper),
            ('/var/run/xgspon-config', self.state),
            ('/var/run/q1000k-xgspon/status.json', self.state / 'optical-status.json'),
            ('/var/run/q1000k-xgspon/lock', self.state / 'optical-lock'),
            ('/etc/config/xgspon', self.root / 'config/xgspon'),
            ('/etc/init.d/xgspon-passthrough', self.root / 'passthrough-init'),
            ('/etc/init.d/xgspon', self.root / 'optical-init'),
        ):
            command = command.replace(str(original), str(replacement))
        self.command = self.backend.write('reload_xgspon_config', command)

    def uci_cmd(self, *args):
        return subprocess.run([str(self.root / 'uci'), *args], env=self.env,
                              text=True, capture_output=True, check=True).stdout

    def apply(self):
        return subprocess.run(['busybox', 'ash', str(self.command)], env=self.env,
                              text=True, capture_output=True, timeout=15)

    def actions(self):
        return self.events.read_text().splitlines() if self.events.exists() else []

    def start_init(self, package, replacements=()):
        source = (ROOT / f'package/network/utils/{package}/files/init').read_text()
        for original, replacement in (
            ('/lib/q1000k-xgspon/apply.sh', self.helper),
            ('/var/run/xgspon-config', self.state),
            *replacements,
        ):
            source = source.replace(str(original), str(replacement))
        init = self.backend.write('test-init', source)
        harness = f'''
procd_open_instance() {{ printf 'open\\n' >> "{self.events}"; }}
procd_set_param() {{ printf 'param:%s\\n' "$*" >> "{self.events}"; }}
procd_close_instance() {{ printf 'close\\n' >> "{self.events}"; }}
. "{init}"
start_service
'''
        return subprocess.run(['busybox', 'ash', '-c', harness], env={
            **self.env, 'Q1000K_CONFIG_APPLY_SNAPSHOT': str(self.root / 'config/xgspon')
        }, text=True, capture_output=True, timeout=15)

    def test_only_changed_section_restarts_and_same_commit_is_noop(self):
        result = self.apply()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.actions(), ['passthrough:stop', 'passthrough:start', 'optical:reload'])
        self.assertEqual(self.apply().returncode, 0)
        self.assertEqual(len(self.actions()), 3)

        self.uci_cmd('set', 'xgspon.passthrough.mode=l3')
        self.uci_cmd('set', 'xgspon.passthrough.client_mac=02:AA:BB:CC:DD:EE')
        self.uci_cmd('commit', 'xgspon')
        self.assertEqual(self.apply().returncode, 0)
        self.assertEqual(self.actions()[-2:], ['passthrough:stop', 'passthrough:start'])
        self.assertEqual(self.actions().count('optical:reload'), 1)

        self.uci_cmd('set', 'xgspon.identity.equipment_id=changed')
        self.uci_cmd('commit', 'xgspon')
        self.assertEqual(self.apply().returncode, 0)
        self.assertEqual(self.actions()[-1], 'optical:reload')
        self.assertEqual(self.actions().count('passthrough:start'), 2)

    def test_uncommitted_and_invalid_settings_cannot_stop_working_services(self):
        self.assertEqual(self.apply().returncode, 0)
        previous = self.actions()
        self.uci_cmd('set', 'xgspon.identity.equipment_id=uncommitted')
        self.assertEqual(self.apply().returncode, 0)
        self.assertEqual(self.actions(), previous)
        self.uci_cmd('set', 'xgspon.identity.registration_id=not-hex')
        self.uci_cmd('commit', 'xgspon')
        result = self.apply()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.actions(), previous)

    def test_invalid_passthrough_rejects_whole_apply_before_optical_stop(self):
        self.assertEqual(self.apply().returncode, 0)
        previous = self.actions()
        self.uci_cmd('set', 'xgspon.identity.equipment_id=new')
        self.uci_cmd('set', 'xgspon.passthrough.mode=l3')
        self.uci_cmd('set', 'xgspon.passthrough.client_mac=ff:ff:ff:ff:ff:ff')
        self.uci_cmd('commit', 'xgspon')
        self.assertNotEqual(self.apply().returncode, 0)
        self.assertEqual(self.actions(), previous)

    def test_failed_passthrough_apply_blocks_optical_reload(self):
        self.assertEqual(self.apply().returncode, 0)
        self.uci_cmd('set', 'xgspon.identity.equipment_id=new')
        self.uci_cmd('set', 'xgspon.passthrough.mode=l3')
        self.uci_cmd('set', 'xgspon.passthrough.client_mac=02:11:22:33:44:55')
        self.uci_cmd('commit', 'xgspon')
        (self.state / 'fail-passthrough').touch()
        self.assertNotEqual(self.apply().returncode, 0)
        self.assertEqual(self.actions()[-1], 'passthrough:stop')
        self.assertEqual(self.actions().count('optical:reload'), 1)

    def test_optical_start_must_reach_running_stage_before_apply_succeeds(self):
        (self.state / 'unhealthy-optical').touch()
        result = self.apply()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('did not reach a healthy stage', result.stderr)
        self.assertFalse((self.state / 'optical.key').exists())
        self.assertEqual(self.actions()[-1], 'optical:reload')

        (self.state / 'unhealthy-optical').unlink()
        result = self.apply()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.actions()[-1], 'optical:reload')
        self.assertTrue((self.state / 'optical.key').exists())

    def test_explicit_apply_restarts_a_dead_optical_owner_with_same_key(self):
        self.assertEqual(self.apply().returncode, 0)
        previous = len(self.actions())
        (self.state / 'optical.running').unlink()
        result = self.apply()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.actions()[previous:], ['optical:reload'])

    def test_disabled_optics_requires_no_running_owner(self):
        self.assertEqual(self.apply().returncode, 0)
        self.uci_cmd('set', 'xgspon.service.enabled=0')
        self.uci_cmd('set', 'xgspon.service.monitor=0')
        self.uci_cmd('commit', 'xgspon')
        (self.state / 'unhealthy-optical').touch()
        result = self.apply()
        self.assertEqual(result.returncode, 0, result.stderr)
        previous = self.actions()
        self.assertEqual(self.apply().returncode, 0)
        self.assertEqual(self.actions(), previous)

    def test_optical_init_directly_owns_run_and_uses_committed_snapshot(self):
        result = self.start_init('q1000k-xgspon-service')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('param:command /usr/libexec/q1000k-xgspon-run', self.actions())
        self.assertTrue((self.state / 'optical.key').exists())
        self.assertIn('option registration_id \'0123\'',
                      (self.state / 'optical-applied').read_text())
        self.events.unlink()
        self.uci_cmd('set', 'xgspon.service.enabled=0')
        self.uci_cmd('set', 'xgspon.service.monitor=1')
        self.uci_cmd('commit', 'xgspon')
        result = self.start_init('q1000k-xgspon-service')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('param:command /usr/libexec/q1000k-xgspon-run --monitor', self.actions())

    def test_passthrough_init_recovers_router_and_starts_direct_l3_owner(self):
        fake = self.backend.write('fake-ipv4', f'''#!/bin/sh
printf 'owner:%s\\n' "$1" >> "{self.events}"
''')
        fake.chmod(0o755)
        replacements = (('/var/run/xgspon-passthrough', self.root / 'owner-state'),
                        ('/usr/libexec/q1000k-passthrough-ipv4', fake))
        result = self.start_init('q1000k-passthrough', replacements)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.actions(), ['owner:--recover-only'])
        self.assertTrue((self.state / 'passthrough.key').exists())
        self.events.unlink()
        self.uci_cmd('set', 'xgspon.passthrough.mode=l3')
        self.uci_cmd('set', 'xgspon.passthrough.client_mac=02:AA:BB:CC:DD:EE')
        self.uci_cmd('commit', 'xgspon')
        result = self.start_init('q1000k-passthrough', replacements)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('owner:--recover-only', self.actions())
        self.assertIn('param:command ' + str(fake) + ' 02:aa:bb:cc:dd:ee', self.actions())


class ApplyJobTests(unittest.TestCase):
    def test_background_apply_reports_result_after_rpcd_style_parent_exits(self):
        with tempfile.TemporaryDirectory(prefix='q1000k-apply-job-') as temporary:
            root = Path(temporary)
            fake = root / 'reload'
            fake.write_text('#!/bin/sh\nsleep 0.1\n[ ! -f "' + str(root / 'fail') + '" ]\n')
            fake.chmod(0o755)
            source = (BASE / 'apply-job').read_text()
            source = source.replace('/var/run/xgspon-apply-jobs', str(root / 'jobs'))
            source = source.replace('/usr/sbin/reload_xgspon_config', str(fake))
            runner = root / 'apply-job'
            runner.write_text(source)
            runner.chmod(0o755)
            for should_fail in (False, True):
                if should_fail:
                    (root / 'fail').touch()
                started = subprocess.run(['busybox', 'ash', str(runner), 'start'],
                                         capture_output=True, text=True, timeout=3, check=True)
                job = json.loads(started.stdout)['job']
                self.assertRegex(job, r'^job-[A-Za-z0-9]+$')
                deadline = time.monotonic() + 3
                while True:
                    status = subprocess.run(['busybox', 'ash', str(runner), 'status', job],
                                            capture_output=True, text=True, timeout=3, check=True)
                    state = json.loads(status.stdout)['state']
                    if state != 'running':
                        break
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.02)
                self.assertEqual(state, 'failed' if should_fail else 'success')
            rejected = subprocess.run(['busybox', 'ash', str(runner), 'status', '../other'],
                                      capture_output=True, text=True)
            self.assertNotEqual(rejected.returncode, 0)

            filter_script = root / 'jsonfilter'
            filter_script.write_text('#!/usr/bin/env python3\nimport json,sys\n'
                                     'print(json.load(sys.stdin)["job"])\n')
            filter_script.chmod(0o755)
            backend_source = (ROOT / 'package/luci-app-econet-xpon/root/usr/libexec/rpcd/econet-xpon').read_text()
            backend = root / 'econet-xpon'
            backend.write_text(backend_source.replace(
                '/usr/libexec/q1000k-xgspon-apply-job', str(runner)))
            env = dict(os.environ, PATH=str(root) + ':' + os.environ['PATH'])
            listed = subprocess.run(['busybox', 'ash', str(backend), 'list'], env=env,
                                    capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(listed.stdout)['applyStatus'], {'job': 'string'})
            started = subprocess.run(['busybox', 'ash', str(backend), 'call', 'apply'], env=env,
                                     capture_output=True, text=True, timeout=3, check=True)
            job = json.loads(started.stdout)['job']
            deadline = time.monotonic() + 3
            while True:
                checked = subprocess.run(['busybox', 'ash', str(backend), 'call', 'applyStatus'],
                                         env=env, input=json.dumps({'job': job}),
                                         capture_output=True, text=True, timeout=3, check=True)
                state = json.loads(checked.stdout)['state']
                if state != 'running':
                    break
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.02)
            self.assertEqual(state, 'failed')


if __name__ == '__main__':
    unittest.main()
