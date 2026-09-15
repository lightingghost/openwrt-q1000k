#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Finish a bench whose final image inspector failed, without rebuilding it.

Only inspector/tool/documentation fixes are allowed since the image commit.
Existing build/test evidence is preserved and verified before it is reused.
Never contacts a device or changes build configuration.
"""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[2]
IMAGE = 'openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(*args):
    return subprocess.check_output(['git', '-C', str(REPO), *args], text=True).strip()


def allowed_change(path):
    return path in ('tests/q1000k/check_pon_bench_image.py',
                    'tests/q1000k/test_pon_bench_revalidate.py',
                    'scripts/q1000k/bench-revalidate.py') or (
        path.endswith('.md') and path.startswith(('scripts/q1000k/', 'target/linux/airoha/')))


def verify_evidence(output):
    entries = {}
    for line in (output / 'sha256sums').read_text().splitlines():
        checksum, name = line.split('  ', 1)
        if Path(name).name != name or name in entries or digest(output / name) != checksum:
            raise ValueError('Saved evidence changed: ' + name)
        entries[name] = checksum
    required = {IMAGE, 'checkpoint.json', 'selection.json', 'build.log', 'resolved.config',
                'kernel.config', 'host-tests.log', 'status-tests.log', 'views-tests.log',
                'inspection.json', IMAGE.replace('-initramfs-bench.itb', '.manifest')}
    if not required <= entries.keys():
        raise ValueError('Saved evidence is incomplete')
    for name in ('host-tests.log', 'status-tests.log'):
        if not (output / name).read_text().rstrip().endswith('\nOK'):
            raise ValueError('Host tests did not pass: ' + name)
    if 'validation passed' not in (output / 'views-tests.log').read_text():
        raise ValueError('View tests did not pass')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact', required=True, type=Path)
    args = parser.parse_args()
    output = args.artifact.resolve(strict=True)
    with (REPO / 'tmp/q1000k-bench-build.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if git('branch', '--show-current') != 'q1000k-xgspon' or git('status', '--porcelain', '--untracked-files=no'):
            raise ValueError('Use a clean q1000k-xgspon checkpoint')
        validation_revision = git('rev-parse', 'HEAD')
        verify_evidence(output)
        checkpoint = output / 'checkpoint.json'
        record = json.loads(checkpoint.read_text())
        revision = record['revision']
        selection = json.loads((output / 'selection.json').read_text())
        if record['status'] != 'failed' or record.get('error') != (
                'Command exited 1; see ' + str(output / 'inspection.json')):
            raise ValueError('Only a final inspector failure can be resumed')
        if record['artifact'] != str(output) or record['source'] != str(REPO) or selection['revision'] != revision:
            raise ValueError('Artifact/source/revision mismatch')
        for path in git('diff', '--name-only', revision, 'HEAD').splitlines():
            if not allowed_change(path):
                raise ValueError('Image or original tests changed; rebuild required: ' + path)
        for name, checksum in record['normal_configs'].items():
            if (digest(REPO / name) if (REPO / name).exists() else None) != checksum:
                raise ValueError('Normal configuration changed: ' + name)
        for name, commit in record['protected_refs'].items():
            if git('rev-parse', name) != commit:
                raise ValueError('Protected reference changed: ' + name)
        data = subprocess.check_output([sys.executable, REPO / 'tests/q1000k/check_pon_bench_image.py',
                                        output / IMAGE, revision], cwd=REPO)
        inspection = json.loads(data)
        root = REPO / 'build_dir/target-aarch64_cortex-a53_musl/root-airoha'
        sums = inspection['runtime_sha256sums']
        for name, checksum in sums.items():
            if Path(name).is_absolute() or '..' in Path(name).parts or digest(root / name) != checksum:
                raise ValueError('Cached runtime differs from the inspected image: ' + name)
        if git('rev-parse', 'HEAD') != validation_revision or git('status', '--porcelain', '--untracked-files=no'):
            raise ValueError('Source changed during validation')
        for name in ('checkpoint.json', 'inspection.json', 'sha256sums'):
            backup = output / (name + '.failed')
            if backup.exists():
                raise ValueError('Prior revalidation evidence already exists: ' + str(backup))
        for name in ('checkpoint.json', 'inspection.json', 'sha256sums'):
            shutil.copy2(output / name, output / (name + '.failed'))
        (output / 'inspection.json').write_bytes(data)
        for name in sums:
            target = output / 'runtime' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / name, target)
        (output / 'runtime-sha256sums').write_text(''.join(h + '  /' + p + '\n' for p, h in sums.items()))
        record.update(status='passed', image=str(output / IMAGE), sha256=digest(output / IMAGE),
                      validation_revision=validation_revision, revalidated=time.time(),
                      saved_host_tests_reused=True, runtime_verified_against_image=True,
                      normal_configs_restored=True, protected_refs_unchanged=True)
        record.pop('error')
        checkpoint.write_text(json.dumps(record, indent=2) + '\n')
        (output / 'sha256sums').write_text(''.join(digest(p) + '  ' + p.name + '\n'
            for p in sorted(output.iterdir()) if p.is_file() and p.name != 'sha256sums'))
        print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
