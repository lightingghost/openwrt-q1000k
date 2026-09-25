#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build and inspect the normal PON initramfs/sysupgrade pair on q1000k-xgspon.

Uses the existing configured build cache, restoring its config and overlay.
Normal images never accept private overlays; device data comes from UBI factory.
No network-device access, flash, upload or branch mutation is performed.
"""
import argparse
import fcntl
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

REPO = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


BUILD = module('bench_build', Path(__file__).with_name('bench-build.py'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New directory under build-artifacts')
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('Use positive --jobs')
    if BUILD.git('branch', '--show-current') != 'q1000k-xgspon':
        raise ValueError('Build only from q1000k-xgspon')
    output = args.output.resolve()
    if not output.is_relative_to(REPO.parent / 'build-artifacts'):
        raise ValueError('Firmware and build records belong under build-artifacts')
    output.mkdir(mode=0o700)
    revision = BUILD.git('rev-parse', 'HEAD')
    snapshot, patch = BUILD.source_state()
    refs = {b: BUILD.git('rev-parse', b) for b in ('main', 'q1000k-dev', 'q1000k-support', 'q1000k-xgspon')}
    (output / 'source.patch').write_bytes(patch)
    (output / 'source-state.json').write_text(json.dumps(snapshot, indent=2) + '\n')
    with tarfile.open(output / 'source-new-files.tar.gz', 'w:gz') as archive:
        for name in snapshot['files']:
            archive.add(REPO / name, arcname=name)
    selected = output / 'selection'
    (selected / 'openwrt/files').mkdir(parents=True)
    shutil.copy2(REPO / 'target/linux/airoha/q1000k-xgspon.config', selected / 'openwrt/.config')
    (selected / 'openwrt/files/build_info').write_text(
        f'Q1000K XGS-PON normal image pair\nBranch: q1000k-xgspon\nRevision: {revision}\n'
        f'Working-tree snapshot: {snapshot["sha256"]}\n')
    backup = output / 'normal-config-backup'
    backup.mkdir()
    record = dict(revision=revision, branch='q1000k-xgspon', source_snapshot=snapshot['sha256'],
                  protected_refs=refs, private=False, status='building', device_access=False)
    checkpoint = output / 'checkpoint.json'
    checkpoint.write_text(json.dumps(record, indent=2) + '\n')
    with (REPO / 'tmp/q1000k-bench-build.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            with BUILD.bench_config(REPO, selected, backup):
                BUILD.run(['make', 'defconfig'], output / 'build.log')
                config = (REPO / '.config').read_text()
                required = ('TARGET_airoha_an7581_DEVICE_quantum_q1000k-ubi',
                            'TARGET_ROOTFS_INITRAMFS', 'TARGET_ROOTFS_SQUASHFS',
                            'PACKAGE_q1000k-xgspon-wan', 'PACKAGE_q1000k-xgspon-service',
                            'PACKAGE_kmod-airoha-xpon-en757x', 'PACKAGE_kmod-q1000k-omci',
                            'PACKAGE_kmod-q1000k-pon-control', 'PACKAGE_luci-app-econet-xpon',
                            'PACKAGE_q1000k-pon-firmware', 'PACKAGE_q1000k-passthrough',
                            'PACKAGE_bridge-hw-offload')
                for key in required:
                    if f'CONFIG_{key}=y\n' not in config:
                        raise ValueError('Kconfig dropped required ' + key)
                for key in ('quantum_q1000k-xgspon-bench', 'quantum_q1000k-xgspon-activation'):
                    if f'CONFIG_TARGET_airoha_an7581_DEVICE_{key}=y\n' in config:
                        raise ValueError('Unexpected RAM bench profile')
                shutil.copy2(REPO / '.config', output / 'resolved.config')
                BUILD.run(['make', '-j' + str(args.jobs), 'V=s'], output / 'build.log')
            if BUILD.source_state()[0] != snapshot:
                raise ValueError('Source changed during build; artifacts are not the recorded snapshot')
            for branch, expected in refs.items():
                if BUILD.git('rev-parse', branch) != expected:
                    raise ValueError('Branch changed during build: ' + branch)
            artifacts = []
            for suffix in ('initramfs-recovery.itb', 'squashfs-sysupgrade.itb'):
                matches = list((REPO / 'bin/targets/airoha/an7581').glob('*-quantum_q1000k-ubi-' + suffix))
                if len(matches) != 1:
                    raise ValueError('Expected exactly one image: ' + suffix)
                image = matches[0]
                shutil.copy2(image, output / image.name)
                artifacts.append(output / image.name)
            for name in ('feeds',):
                record['feed_revisions'] = {p.name: subprocess.check_output(
                    ['git', '-C', str(p), 'rev-parse', 'HEAD'], text=True).strip()
                    for p in (REPO / name).iterdir() if p.is_dir() and (p / '.git').exists()}
            command = [sys.executable, REPO / 'tests/q1000k/check_pon_normal_images.py',
                       '--revision', revision, '--snapshot', snapshot['sha256'], '--output', output / 'inspection.json']
            BUILD.run(command + artifacts, output / 'inspection.log')
            (output / 'sha256sums').write_text(''.join(BUILD.digest(p) + '  ' + p.name + '\n' for p in artifacts))
            record['status'] = 'built-and-inspected'
        except BaseException:
            record['status'] = 'failed'
            raise
        finally:
            checkpoint.write_text(json.dumps(record, indent=2) + '\n')
    print('Built and inspected both images in ' + str(output))


if __name__ == '__main__':
    main()
