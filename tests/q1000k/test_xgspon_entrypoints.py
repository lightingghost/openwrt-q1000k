#!/usr/bin/env python3
"""Canonical init boot links and compatibility delegation without hardware."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class EntrypointTests(unittest.TestCase):
    def test_offline_init_enable_creates_only_canonical_boot_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = ROOT / 'package/base-files/files'
            for name in ['etc/rc.common', 'lib/functions.sh', 'lib/functions/service.sh']:
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(base / name, target)
            # The offline enable action never calls procd; no ubus on the host.
            (root / 'lib/functions/procd.sh').touch()
            (root / 'etc/rc.d').mkdir()
            (root / 'etc/init.d').mkdir()
            env = dict(os.environ, IPKG_INSTROOT=str(root))
            for package, canonical in [('q1000k-xgspon-service', 'xgspon'),
                                       ('q1000k-passthrough', 'xgspon-passthrough')]:
                for source, name in [('init', canonical), ('legacy-init', 'q1000k-' + canonical.removeprefix('xgspon-'))]:
                    script = root / 'etc/init.d' / name
                    shutil.copy2(ROOT / 'package/network/utils' / package / 'files' / source, script)
                    run = subprocess.run(['bash', str(root/'etc/rc.common'), str(script), 'enable'],
                                         env=env, capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(sorted(p.name for p in (root/'etc/rc.d').iterdir()),
                             ['K10xgspon', 'S95xgspon', 'S96xgspon-passthrough'])

    def test_legacy_init_forwards_actions_to_canonical_service(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for package, canonical in [('q1000k-xgspon-service', 'xgspon'),
                                       ('q1000k-passthrough', 'xgspon-passthrough')]:
                target = root / canonical
                target.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
                target.chmod(0o755)
                source = (ROOT/'package/network/utils'/package/'files/legacy-init').read_text()
                source = source.replace('/etc/init.d/'+canonical, str(target))
                script = root/'alias'
                script.write_text(source)
                env = dict(os.environ)
                env.pop('IPKG_INSTROOT', None)
                for action in ['enable', 'start', 'stop', 'restart', 'reload', 'stop_monitor']:
                    run = subprocess.run(['sh', str(script), action], env=env,
                                         text=True, capture_output=True)
                    self.assertEqual(run.returncode, 0, run.stderr)
                    self.assertEqual(run.stdout.strip(), action)

    def test_xgspon_cli_delegates_lifecycle_to_canonical_service(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/'common').touch()
            target = root/'service'
            target.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
            target.chmod(0o755)
            source = (ROOT/'package/network/utils/q1000k-xgspon/files/q1000k-xgspon').read_text()
            source = source.replace('/lib/q1000k-xgspon/common.sh', str(root/'common'))
            source = source.replace('/etc/init.d/xgspon', str(target))
            script = root/'xgspon'
            script.write_text(source)
            for action in ['start', 'stop', 'restart', 'reload']:
                run = subprocess.run(['sh', str(script), action], text=True, capture_output=True)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual(run.stdout.strip(), action)


if __name__ == '__main__':
    unittest.main()
