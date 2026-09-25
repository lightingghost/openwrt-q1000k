#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build, inspect and record a pinned RAM bench using the existing build cache.

Run from an idle OpenWrt checkout. This script never accesses a device.
"""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tarfile
import time

REPO = Path(__file__).resolve().parents[2]
IMAGE = 'openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb'
MANIFEST = 'openwrt-airoha-an7581-quantum_q1000k-xgspon-bench.manifest'


def git(*args):
    return subprocess.check_output(['git', '-C', str(REPO), *args], text=True).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_state():
    """Hash tracked changes and nonignored new files; never modify the index."""
    patch = subprocess.check_output(['git', '-C', str(REPO), 'diff', '--binary', 'HEAD'])
    names = subprocess.check_output(['git', '-C', str(REPO), 'ls-files', '--others',
                                     '--exclude-standard', '-z']).decode().split('\0')
    files = {}
    for name in sorted(filter(None, names)):
        path = REPO / name
        if path.is_symlink():
            target = os.readlink(path)
            if not path.resolve().is_relative_to(REPO) or not path.is_file():
                raise ValueError('Snapshot symlink must resolve to a repository file: ' + name)
            files[name] = dict(symlink=target, sha256=hashlib.sha256(target.encode()).hexdigest())
            continue
        if not path.is_file():
            raise ValueError('Snapshot requires regular source files: ' + name)
        files[name] = dict(sha256=digest(path), mode=path.stat().st_mode & 0o777)
    state = dict(patch_sha256=hashlib.sha256(patch).hexdigest(), files=files)
    state['sha256'] = hashlib.sha256(json.dumps(state, sort_keys=True).encode()).hexdigest()
    return state, patch


def run(command, log):
    print('+ ' + ' '.join(map(str, command)), flush=True)
    with log.open('ab') as output:
        process = subprocess.Popen(list(map(str, command)), cwd=REPO,
                                   stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            result = process.wait()
        except BaseException:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=45)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            raise
    if result:
        raise RuntimeError(f'Command exited {result}; see {log}')


@contextmanager
def bench_config(repo, selected, backup, private_overlay=None):
    """Restore both normal configs and remove only our owned overlay on failure."""
    overlay = repo / 'files'
    if overlay.exists() or overlay.is_symlink():
        raise ValueError('Existing files overlay must be preserved; use another checkout')
    saved = {}
    for name in ('.config', '.config.old'):
        path = repo / name
        if path.is_symlink():
            raise ValueError('Refuse symlinked configuration: ' + name)
        saved[name] = path.exists()
        if path.exists():
            shutil.copy2(path, backup / name)
    if not saved['.config']:
        raise ValueError('This wrapper requires an existing configured build cache')
    overlay.mkdir()
    owned = []
    directories = []
    try:
        shutil.copy2(selected / 'openwrt/files/build_info', overlay / 'build_info')
        owned.append((overlay / 'build_info', None))
        if private_overlay is not None:
            for source in sorted(private_overlay.rglob('*')):
                if source.is_symlink():
                    raise ValueError('No symlinks in private overlay')
                target = overlay / source.relative_to(private_overlay)
                if source.is_dir():
                    target.mkdir(mode=0o755)
                    directories.append(target)
                elif source.is_file():
                    if target.exists():
                        raise ValueError('Private overlay collides with build metadata')
                    shutil.copy2(source, target)
                    owned.append((target, digest(target)))
                else:
                    raise ValueError('Non-regular private overlay entry')
        shutil.copy2(selected / 'openwrt/.config', repo / '.config')
        yield
    finally:
        for name, existed in saved.items():
            if existed:
                shutil.copy2(backup / name, repo / name)
            else:
                (repo / name).unlink(missing_ok=True)
        for path, checksum in reversed(owned):
            if path.exists():
                if checksum is not None and digest(path) != checksum:
                    raise ValueError('Private overlay changed during build; retained')
                path.unlink()
        for path in reversed(directories):
            path.rmdir()
        overlay.rmdir()  # Preserve/report any concurrently added files.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--builder', type=Path, default=REPO.parent / 'q1000k-build-xgspon')
    parser.add_argument('--output', required=True, type=Path, help='New artifact directory')
    parser.add_argument('--profile', choices=('bench', 'activation'), default='bench')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--test-jobs', type=int,
                        help='Host test workers (default: --jobs, capped at 8; 1 for serial debugging)')
    parser.add_argument('--working-tree', action='store_true',
                        help='Preserve a hashed source snapshot without committing shared agent work')
    parser.add_argument('--private-inputs', type=Path, help='Approved firmware/calibration archive; activation only')
    parser.add_argument('--private-identity', type=Path, help='Approved identity JSON; enables continuous private service')
    args = parser.parse_args()
    image_name = IMAGE.replace('xgspon-bench-initramfs', 'xgspon-activation-initramfs') if args.profile == 'activation' else IMAGE
    manifest_name = MANIFEST.replace('xgspon-bench.manifest', 'xgspon-activation.manifest') if args.profile == 'activation' else MANIFEST
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.test_jobs is None:
        args.test_jobs = min(args.jobs, 8)
    if args.test_jobs < 1:
        parser.error('--test-jobs must be positive')
    if bool(args.private_inputs) != bool(args.private_identity) or (args.private_inputs and args.profile != 'activation'):
        parser.error('Private autostart requires both input files and --profile activation')
    if git('branch', '--show-current') != 'q1000k-xgspon':
        raise ValueError('Build only from q1000k-xgspon')
    if git('status', '--porcelain', '--untracked-files=no') and not args.working_tree:
        raise ValueError('Commit tracked changes before selecting a build checkpoint')
    snapshot, patch = source_state()
    revision = git('rev-parse', 'HEAD')
    builder = args.builder.resolve()
    if subprocess.check_output(['git', '-C', str(builder), 'status', '--porcelain'], text=True):
        raise ValueError('Experimental builder must be clean')
    output = args.output.resolve()
    output.mkdir(mode=0o700)
    private_overlay = None
    if args.private_inputs:
        spec = importlib.util.spec_from_file_location('private_autostart', Path(__file__).with_name('private-autostart.py'))
        private = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(private)
        private_overlay = output / 'private-overlay'
        manifest = private.generate(args.private_inputs, args.private_identity, private_overlay)
        (output / 'private-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        # Initramfs intermediates contain the same private inputs as the ITB.
        (REPO / 'build_dir/target-aarch64_cortex-a53_musl').chmod(0o700)
    if args.working_tree:
        (output / 'source.patch').write_bytes(patch)
        (output / 'source-state.json').write_text(json.dumps(snapshot, indent=2) + '\n')
        with tarfile.open(output / 'source-new-files.tar.gz', 'w:gz') as archive:
            for name in snapshot['files']:
                archive.add(REPO / name, arcname=name)
    backup = output / 'normal-config-backup'
    backup.mkdir()
    selection = output / 'selection-work'
    record = dict(schema_version=1, revision=revision, branch='q1000k-xgspon', profile=args.profile,
                  test_jobs=args.test_jobs,
                  source=str(REPO), artifact=str(output), started=time.time(), status='building',
                  source_state=snapshot if args.working_tree else None,
                  builder=str(builder), builder_revision=subprocess.check_output(
                      ['git', '-C', str(builder), 'rev-parse', 'HEAD'], text=True).strip(),
                  protected_refs={b: git('rev-parse', b) for b in ('main', 'q1000k-dev', 'q1000k-support')},
                  normal_configs={n: digest(REPO / n) if (REPO / n).exists() else None
                                  for n in ('.config', '.config.old')})
    checkpoint = output / 'checkpoint.json'
    checkpoint.write_text(json.dumps(record, indent=2) + '\n')
    with (REPO / 'tmp/q1000k-bench-build.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            tool = builder / 'scripts/q1000k-xgspon-build.py'
            run([sys.executable, tool, 'prepare', '--profile', args.profile, '--repo', REPO,
                 '--revision', revision, selection], output / 'prepare.log')
            with bench_config(REPO, selection, backup, private_overlay):
                if args.working_tree:
                    with (REPO / 'files/build_info').open('a') as info:
                        info.write('Working-tree snapshot: ' + snapshot['sha256'] + '\n')
                run(['make', 'defconfig'], output / 'build.log')
                shutil.copy2(REPO / '.config', output / 'resolved.config')
                shutil.copy2(REPO / '.config', selection / 'openwrt/.config')
                run([sys.executable, tool, 'verify', selection], output / 'build.log')
                run(['make', '-j' + str(args.jobs), 'V=s'], output / 'build.log')
                if private_overlay is not None:
                    for name in (image_name, manifest_name):
                        (REPO / 'bin/targets/airoha/an7581' / name).chmod(0o600)
            if git('rev-parse', 'HEAD') != revision or source_state()[0] != snapshot:
                raise ValueError('Source changed during build')
            run([sys.executable, REPO / 'tests/q1000k/run_host_tests.py',
                 '--jobs', str(args.test_jobs), '--results', output / 'host-tests.json'],
                output / 'host-tests.log')
            run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tests/q1000k',
                 '-p', 'test_xgspon.py'], output / 'status-tests.log')
            run(['node', REPO / 'tests/q1000k/test_xgspon_views.cjs'], output / 'views-tests.log')
            run(['node', REPO / 'tests/q1000k/test_xgspon_ports.cjs'], output / 'ports-tests.log')
            for name in (image_name, manifest_name):
                if private_overlay is not None:
                    (REPO / 'bin/targets/airoha/an7581' / name).chmod(0o600)
                shutil.copy2(REPO / 'bin/targets/airoha/an7581' / name, output / name)
            shutil.copy2(selection / 'selection.json', output / 'selection.json')
            kernels = list((REPO / 'build_dir/target-aarch64_cortex-a53_musl/linux-airoha_an7581').glob('linux-*/.config'))
            if len(kernels) != 1:
                raise ValueError('Kernel configuration is ambiguous')
            shutil.copy2(kernels[0], output / 'kernel.config')
            # Retain the ELF so a host collector can pin /sys/kernel/notes to
            # the exact kernel embedded in the delivered FIT after packaging.
            shutil.copy2(kernels[0].parent.parent / 'vmlinux-initramfs.debug', output / 'vmlinux-initramfs.debug')
            inspect = [sys.executable, REPO / 'tests/q1000k/check_pon_bench_image.py',
                       output / image_name, revision, '--profile', args.profile]
            if private_overlay is not None:
                inspect.extend(['--private-manifest', output / 'private-manifest.json'])
            run(inspect, output / 'inspection.json')
            root = REPO / 'build_dir/target-aarch64_cortex-a53_musl/root-airoha'
            paths = ['usr/sbin/q1000k-pon-bench', 'usr/sbin/q1000k-pon-passthrough', 'usr/sbin/q1000k-ip-passthrough', 'usr/sbin/q1000k-l3-bench',
                     'usr/libexec/q1000k-passthrough-ipv4', 'usr/libexec/q1000k-ipv6-transition', 'usr/libexec/q1000k-passthrough-watch-config', 'etc/init.d/q1000k-passthrough', 'etc/hotplug.d/iface/95-q1000k-passthrough', 'lib/q1000k-xgspon/common.sh',
                     'usr/share/libubox/jshn.sh', 'usr/sbin/q1000k-omci', 'usr/libexec/q1000k-omci-config',
                     'usr/sbin/q1000k-pon-factory', 'usr/sbin/q1000k-xgspon',
                     'usr/libexec/q1000k-xgspon-run', 'usr/libexec/q1000k-xgspon-watch-config', 'etc/init.d/q1000k-xgspon',
                     'usr/share/luci/menu.d/luci-app-econet-xpon.json',
                     'www/luci-static/resources/preload/xgspon-ports.js',
                     'www/luci-static/resources/view/econet-xpon/status-v4.js',
                     'www/luci-static/resources/view/econet-xpon/settings-v5.js']
            if args.profile == 'activation':
                paths.extend(['usr/sbin/q1000k-pon-validate', 'usr/libexec/q1000k-pd-source', 'usr/libexec/q1000k-ipv6-bench', 'usr/libexec/q1000k-ipv6-client', 'usr/libexec/q1000k-udp6-probe', 'usr/share/q1000k-bench/capabilities.json'])
            for name in ('q1000k-pon-control', 'airoha_ecnt_hook', 'airoha_ecnt_scu',
                         'airoha_ecnt_pon_phy', 'airoha_ecnt_xpon', 'phy_10g', 'xpon', 'omci', 'xpon_10g'):
                modules = list(root.glob('lib/modules/*/' + name + '.ko'))
                if len(modules) != 1:
                    raise ValueError('Missing/ambiguous module: ' + name)
                paths.append(str(modules[0].relative_to(root)))
            (output / 'runtime-sha256sums').write_text(''.join(digest(root / p) + '  /' + p + '\n' for p in paths))
            for path in paths:
                target = output / 'runtime' / path
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(root / path, target)
            record.update(status='passed', image=str(output / image_name), sha256=digest(output / image_name))
        except BaseException as error:
            record.update(status='failed', error=str(error))
            raise
        finally:
            record['normal_configs_restored'] = all(
                (digest(REPO / n) if (REPO / n).exists() else None) == h
                for n, h in record['normal_configs'].items())
            record['protected_refs_unchanged'] = all(git('rev-parse', n) == h for n, h in record['protected_refs'].items())
            if not record['normal_configs_restored'] or not record['protected_refs_unchanged']:
                record['status'] = 'failed'
            record['finished'] = time.time()
            checkpoint.write_text(json.dumps(record, indent=2) + '\n')
            (output / 'sha256sums').write_text(''.join(digest(p) + '  ' + p.name + '\n'
                for p in sorted(output.iterdir()) if p.is_file() and p.name != 'sha256sums'))
    print(json.dumps(record, indent=2))
    return 0 if record['status'] == 'passed' else 1


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'Interrupted by signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, interrupted)
    raise SystemExit(main())
