#!/usr/bin/env python3
"""Exercise host-side rejection before any device command is issued."""
import hashlib
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('bench_run', ROOT / 'scripts/q1000k/bench-run.py')
RUN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN)


class RunnerTests(unittest.TestCase):
    def test_input_archive_rejects_links_duplicates_and_traversal(self):
        content = b'private fixture'
        digest = hashlib.sha256(content).hexdigest()
        inputs = {n: (len(content), digest) for n in RUN.INPUTS}
        with tempfile.TemporaryDirectory() as directory, patch.object(RUN, 'INPUTS', inputs):
            archive = Path(directory) / 'input.tar'
            for problem in ('none', 'symlink', 'duplicate', 'traversal', 'hash'):
                with self.subTest(problem=problem), tarfile.open(archive, 'w') as tar:
                    files = {n: content for n in inputs}
                    files['sha256sums'] = ''.join(f'{digest}  {n}\n' for n in inputs).encode()
                    for i, (name, data) in enumerate(files.items()):
                        if problem == 'traversal' and i == 0:
                            name = '../' + name
                        if problem == 'hash' and i == 0:
                            data = b'wrong hash'
                        member = tarfile.TarInfo(name)
                        member.size = len(data)
                        if problem == 'symlink' and i == 0:
                            member.type = tarfile.SYMTYPE
                            member.linkname = '/etc/shadow'
                        tar.addfile(member, io.BytesIO(data))
                        if problem == 'duplicate' and i == 0:
                            tar.addfile(member, io.BytesIO(data))
                if problem == 'none':
                    RUN.validate_inputs(archive)
                else:
                    with self.assertRaises(ValueError):
                        RUN.validate_inputs(archive)

    def test_runtime_manifest_requires_every_module_without_shell_syntax(self):
        names = ['/usr/sbin/q1000k-pon-bench', '/lib/q1000k-xgspon/common.sh',
                 '/usr/share/libubox/jshn.sh', '/usr/sbin/q1000k-omci']
        names += ['/lib/modules/6.18.44/' + (n.replace('_', '-') if n == 'q1000k_pon_control' else n)
                  + '.ko' for n in RUN.MODULES]
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / 'sums'
            for entries in (names, names[:-1], names + names[:1], names[:-1] + ['/tmp/$(id)']):
                manifest.write_text(''.join('a' * 64 + '  ' + n + '\n' for n in entries))
                if entries == names:
                    RUN.runtime_manifest(manifest)
                else:
                    with self.assertRaises(ValueError):
                        RUN.runtime_manifest(manifest)

    def test_default_command_cannot_contact_working_router(self):
        self.assertEqual(RUN.SSH[-1], 'root@192.168.255.1')
        self.assertNotIn('192.168.1.1', ' '.join(RUN.SSH))

class ModuleRetryTests(unittest.TestCase):
    def test_module_retry_checks_kernel_sources_dependencies_and_payloads(self):
        with tempfile.TemporaryDirectory() as directory:
            old, new = Path(directory) / 'old', Path(directory) / 'new'
            for path, revision in ((old, 'a' * 40), (new, 'b' * 40)):
                path.mkdir()
                (path / 'selection.json').write_text(__import__('json').dumps({'revision': revision}))
                (path / 'kernel.config').write_text('same kernel')
            names = ['/usr/sbin/q1000k-pon-bench', '/lib/q1000k-xgspon/common.sh',
                     '/usr/share/libubox/jshn.sh', '/usr/sbin/q1000k-omci']
            names += ['/lib/modules/6.18.44/' + (n.replace('_', '-') if n == 'q1000k_pon_control' else n)
                      + '.ko' for n in RUN.MODULES]
            sums = ''.join('a' * 64 + '  ' + n + '\n' for n in names)
            updated = sums
            for name in ('airoha_ecnt_xpon.ko', 'xpon_10g.ko'):
                path = new / 'runtime/lib/modules/6.18.44' / name
                path.parent.mkdir(parents=True, exist_ok=True)
                data = b'\x7fELFfixture' + name.encode()
                path.write_bytes(data)
                updated = updated.replace('a' * 64 + '  /lib/modules/6.18.44/' + name,
                                          hashlib.sha256(data).hexdigest() + '  /lib/modules/6.18.44/' + name)
            (new / 'runtime-sha256sums').write_text(updated)
            with patch.object(RUN.subprocess, 'check_output', return_value='scripts/q1000k/bench-run.py\n'):
                result = RUN.module_update(old, new, sums)
                self.assertEqual(len(result[2]), 2)
                (new / 'kernel.config').write_text('different kernel')
                with self.assertRaisesRegex(ValueError, 'kernel configuration'):
                    RUN.module_update(old, new, sums)
                (new / 'kernel.config').write_text('same kernel')
                (new / 'runtime-sha256sums').write_text(updated.replace(
                    'a' * 64 + '  /lib/modules/6.18.44/phy_10g.ko',
                    'b' * 64 + '  /lib/modules/6.18.44/phy_10g.ko'))
                with self.assertRaisesRegex(ValueError, 'dependency changed'):
                    RUN.module_update(old, new, sums)
                (new / 'runtime-sha256sums').write_text(updated)
                (new / 'runtime/lib/modules/6.18.44/xpon_10g.ko').write_bytes(b'corrupt')
                with self.assertRaisesRegex(ValueError, 'hash/format'):
                    RUN.module_update(old, new, sums)
            with patch.object(RUN.subprocess, 'check_output', return_value='target/linux/airoha/patches-6.18/new.patch\n'):
                with self.assertRaisesRegex(ValueError, 'new RAM boot'):
                    RUN.module_update(old, new, sums)


BUILD_SPEC = importlib.util.spec_from_file_location('bench_build', ROOT / 'scripts/q1000k/bench-build.py')
BUILD = importlib.util.module_from_spec(BUILD_SPEC)
BUILD_SPEC.loader.exec_module(BUILD)


class BuildRestoreTests(unittest.TestCase):
    def test_failed_build_restores_configs_and_removes_only_owned_overlay(self):
        for has_old in (False, True):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                selected = root / 'selection'
                (selected / 'openwrt/files').mkdir(parents=True)
                (selected / 'openwrt/files/build_info').write_text('bench')
                (selected / 'openwrt/.config').write_text('bench-config')
                backup = root / 'backup'
                backup.mkdir()
                (root / '.config').write_text('original')
                if has_old:
                    (root / '.config.old').write_text('old')
                with self.assertRaisesRegex(RuntimeError, 'build failed'):
                    with BUILD.bench_config(root, selected, backup):
                        self.assertEqual((root / '.config').read_text(), 'bench-config')
                        (root / '.config.old').write_text('changed')
                        raise RuntimeError('build failed')
                self.assertEqual((root / '.config').read_text(), 'original')
                self.assertEqual((root / '.config.old').exists(), has_old)
                if has_old:
                    self.assertEqual((root / '.config.old').read_text(), 'old')
                self.assertFalse((root / 'files').exists())

    def test_existing_overlay_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'files').mkdir()
            (root / 'files/owned-by-user').write_text('keep')
            with self.assertRaises(ValueError):
                with BUILD.bench_config(root, root, root):
                    self.fail('Must refuse existing overlay')
            self.assertEqual((root / 'files/owned-by-user').read_text(), 'keep')


if __name__ == '__main__':
    unittest.main()
