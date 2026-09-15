#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Capture a verified Q1000K RAM bench run. Never boots or flashes a device."""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import tarfile
import time

PROBES = ('bit-order', 'descrambler', 'fec-oc', 'fec-off', 'gain-auto', 'gain-low',
          'tdc-delay', 'pll-order', 'oem-order', 'checker')
HOST = '192.168.255.1'
SSH = ['ssh', '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null',
       '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', '-o', 'ServerAliveInterval=5',
       '-o', 'ServerAliveCountMax=2', 'root@' + HOST]
MODULES = ('q1000k_pon_control airoha_ecnt_hook airoha_ecnt_scu airoha_ecnt_pon_phy '
           'airoha_ecnt_xpon phy_10g xpon omci xpon_10g').split()
INPUTS = {
    'xgspon-calibration.bin': (513, 'f2ec3b0de9683d113755d5d4df4fcafe8a4b47a43153ad0de45cbe9cd34c6e1c'),
    'A60993.elf.pm': (15232, '5a8a4bbae5f70c1e615ba0aa1c2a1dce654611d3205d2fa983bf41e6cdadb4a1'),
    'A60993.elf.dm': (56, '21618dc3694a1e6f6b28c7da7141964dea1d6e57f2d2956bbe72a780ca6166a4'),
}
FIRMWARE = '/lib/firmware/airoha/q1000k'
REPO = Path(__file__).resolve().parents[2]


@contextmanager
def device_lock():
    """Serialize staging, hardware ownership and cleanup across host runners."""
    with (REPO / 'tmp/q1000k-bench-device.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def module_update(original, replacement, sums):
    """Only the vendor PHY/MAC/provider may change on an existing kernel."""
    before = json.loads((original / 'selection.json').read_text())['revision']
    after = json.loads((replacement / 'selection.json').read_text())['revision']
    if not re.fullmatch('[0-9a-f]{40}', after):
        raise ValueError('Invalid replacement source revision')
    if (original / 'kernel.config').read_bytes() != (replacement / 'kernel.config').read_bytes():
        raise ValueError('Replacement requires a different kernel configuration; RAM boot it instead')
    changed = subprocess.check_output(['git', '-C', str(REPO), 'diff', '--name-only',
                                       before, after], text=True).splitlines()
    for name in changed:
        if not (name.startswith(('package/kernel/airoha-pon/', 'tests/q1000k/', 'scripts/q1000k/',
                                 'package/network/utils/q1000k-xgspon-bench/')) or
                (name.startswith('target/linux/airoha/') and name.endswith('.md'))):
            raise ValueError('Source change requires a new RAM boot: ' + name)
    old = {p: h for h, p in (line.split() for line in sums.splitlines())}
    new = {p: h for h, p in (line.split() for line in runtime_manifest(replacement / 'runtime-sha256sums').splitlines())}
    allowed = {'airoha_ecnt_xpon.ko', 'xpon_10g.ko', 'phy_10g.ko'}
    if old.keys() != new.keys():
        raise ValueError('Replacement runtime paths differ')
    active, updates = dict(old), {}
    for path, checksum in new.items():
        if Path(path).name not in allowed:
            if path.endswith('.ko') and old[path] != checksum:
                raise ValueError('Additional dependency changed: ' + path)
            continue
        payload = (replacement / 'runtime' / path.lstrip('/')).read_bytes()
        if not payload.startswith(b'\x7fELF') or hashlib.sha256(payload).hexdigest() != checksum:
            raise ValueError('Replacement module hash/format mismatch: ' + path)
        if checksum != old[path]:
            updates[path] = payload
            active[path] = checksum
    if not updates:
        raise ValueError('No diagnostic modules changed')
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode='w') as archive:
        for path, payload in updates.items():
            member = tarfile.TarInfo(Path(path).name)
            member.size, member.mode = len(payload), 0o600
            archive.addfile(member, io.BytesIO(payload))
    return after, ''.join(f'{h}  {p}\n' for p, h in active.items()), updates, data.getvalue()


def validate_inputs(path):
    """Accept only this bench unit's previously verified, flat regular files."""
    with tarfile.open(path, 'r:') as archive:
        members = archive.getmembers()
        if (len(members) != 4 or {m.name for m in members} != set(INPUTS) | {'sha256sums'}
                or any(not m.isfile() or m.size > 16384 for m in members)):
            raise ValueError('Unexpected private input archive members')
        for name, (size, digest) in INPUTS.items():
            data = archive.extractfile(name).read()
            if len(data) != size or hashlib.sha256(data).hexdigest() != digest:
                raise ValueError('Private input size/hash mismatch: ' + name)
        sums = archive.extractfile('sha256sums').read().decode()
        if sorted(sums.splitlines()) != sorted(f'{h}  {n}' for n, (_, h) in INPUTS.items()):
            raise ValueError('Unexpected private input checksum manifest')


def runtime_manifest(path):
    data = path.read_text()
    names = []
    for line in data.splitlines():
        match = re.fullmatch(r'[0-9a-f]{64}  (/[-A-Za-z0-9_./]+)', line)
        if not match or '..' in Path(match[1]).parts:
            raise ValueError('Invalid runtime checksum manifest')
        names.append(match[1])
    expected = {'/usr/sbin/q1000k-pon-bench', '/lib/q1000k-xgspon/common.sh',
                '/usr/share/libubox/jshn.sh', '/usr/sbin/q1000k-omci', '/usr/libexec/q1000k-omci-config'}
    expected_modules = {m.replace('_', '-') if m == 'q1000k_pon_control' else m for m in MODULES}
    modules = [p for p in names if re.fullmatch(r'/lib/modules/[0-9][0-9A-Za-z.+-]*/[-a-z0-9_]+\.ko', p)]
    if (len(names) != 14 or len(set(names)) != 14 or set(names) - set(modules) != expected
            or {Path(p).stem for p in modules} != expected_modules):
        raise ValueError('Runtime manifest must identify the helper and all nine PON modules')
    return data


def guards(revision, sums):
    return f'''set -eu
q1000k-pon-bench status
 grep -qx {shlex.quote('Revision: ' + revision)} /build_info
 test -n "$(find /sys/firmware/devicetree/base -name quantum,tx-inhibit -type f)"
sha256sum -c <<'Q1000K_RUNTIME_SUMS'
{sums.rstrip()}
Q1000K_RUNTIME_SUMS
'''


def idle_guards():
    return f'''for module in {' '.join(MODULES)}; do
    test ! -d /sys/module/$module
done
 test ! -e /var/run/q1000k-pon-bench.lock
 test ! -e /sys/class/net/ponraw/master
flags=$(cat /sys/class/net/ponraw/flags)
 test "$((flags & 1))" = 0
'''


def ssh(script, log, payload=None, timeout=120):
    command = SSH + (['sh', '-s'] if payload is None else [script])
    with log.open('wb') as output:
        result = subprocess.run(command, input=script.encode() if payload is None else payload,
                                stdout=output, stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'SSH failed ({result.returncode}); see {log}')


def resource_probe():
    """Load only providers whose probe reads/maps resources without reset writes."""
    return '''printf '0\\n' > /proc/sys/kernel/panic
test "$(cat /proc/sys/kernel/panic)" = 0
owned=''
cleanup() {
    result=$?
    trap - EXIT
    for module in $owned; do
        rmmod "$module" || exit 1
    done
    exit "$result"
}
trap cleanup EXIT
for module in airoha_ecnt_hook airoha_ecnt_scu airoha_ecnt_xpon; do
    insmod /lib/modules/$(uname -r)/$module.ko
    test -d /sys/module/$module
    owned="$module $owned"
done
dmesg
'''


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['status', 'resources', 'controller', 'stack', 'receive'])
    parser.add_argument('--artifact', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path, help='New private capture directory')
    parser.add_argument('--serial-log', type=Path, default=Path('/tmp/serial_output.log'))
    parser.add_argument('--inputs', type=Path, help='Verified private tar; never copied to the repository')
    fiber = parser.add_mutually_exclusive_group()
    fiber.add_argument('--fiber-disconnected', action='store_true')
    fiber.add_argument('--fiber-connected', action='store_true', help='Only for the explicit receive-only test')
    parser.add_argument('--reacquire-once', action='store_true',
                        help='Connected receive only: opt into one bounded PMA out/in recovery')
    parser.add_argument('--restore-pll', action='store_true',
                        help='Restore PHY PLL clocks after the single connected RX recovery')
    parser.add_argument('--restore-gain', action='store_true',
                        help='Apply the OEM RX frontend gain after the single recovery')
    parser.add_argument('--probe', choices=PROBES, help='One bounded, checked RX experiment')
    parser.add_argument('--samples', type=int, choices=(30, 90, 180),
                        help='Receive observations at one-second intervals (default: 30)')
    parser.add_argument('--modules-from', type=Path, help='Verified newer artifact; temporarily replace only PHY/MAC/provider modules in RAM')
    parser.add_argument('--registers', action='store_true', help='Read only the fixed SCU/MAC configuration register list during status')
    args = parser.parse_args(argv)
    if args.probe and (not args.reacquire_once or args.restore_gain or args.restore_pll):
        parser.error('--probe requires exclusive --reacquire-once')
    if args.restore_gain and not args.reacquire_once:
        parser.error('--restore-gain requires --reacquire-once')
    if args.restore_pll and not args.reacquire_once:
        parser.error('--restore-pll requires --reacquire-once')
    if args.samples is not None and args.action != 'receive':
        parser.error('--samples is only for receive')
    if args.reacquire_once and (args.action != 'receive' or not args.fiber_connected):
        parser.error('--reacquire-once requires receive --fiber-connected')
    if args.fiber_connected and args.action != 'receive':
        parser.error('--fiber-connected is only valid for receive')
    if args.action != 'status' and not (args.fiber_disconnected or args.fiber_connected):
        parser.error('RAM module tests require an explicit fiber state')
    if args.action in ('controller', 'stack', 'receive') and args.inputs is None:
        parser.error('Tests require --inputs; status is read-only')
    if args.registers and args.action != 'status':
        parser.error('--registers is only for a read-only status capture')
    with device_lock():
        return execute(args)


def execute(args):
    artifact = args.artifact.resolve()
    revision = json.loads((artifact / 'selection.json').read_text())['revision']
    if not re.fullmatch('[0-9a-f]{40}', revision):
        raise ValueError('Invalid artifact revision')
    sums = runtime_manifest(artifact / 'runtime-sha256sums')
    if args.action in ('controller', 'stack', 'receive'):
        validate_inputs(args.inputs)
    update = None
    if args.modules_from:
        if args.action not in ('resources', 'stack'):
            raise ValueError('--modules-from is only for a resources or stack test')
        update = module_update(artifact, args.modules_from.resolve(), sums)
    output = args.output.resolve()
    remote = '/tmp/q1000k-bench-' + output.name
    if not re.fullmatch(r'/tmp/q1000k-bench-[-A-Za-z0-9_.]+', remote):
        raise ValueError('Use a simple output directory name')
    serial = args.serial_log.open('rb')  # Retain the descriptor across rotation.
    output.mkdir(mode=0o700)  # Never overwrite a prior capture.
    start = serial.seek(0, 2)
    fiber_flag = '--fiber-connected' if args.fiber_connected else '--fiber-disconnected'
    recovery_flag = ' --reacquire-once' if args.reacquire_once else ''
    if args.restore_pll:
        recovery_flag += ' --restore-pll'
    if args.restore_gain:
        recovery_flag += ' --restore-gain'
    if getattr(args, 'probe', None):
        recovery_flag += ' --probe ' + shlex.quote(args.probe)
    sample_flag = f' --samples {args.samples}' if args.samples is not None else ''
    run = dict(schema_version=1, action=args.action, host=HOST, revision=revision,
               fiber='connected' if args.fiber_connected else 'disconnected' if args.fiber_disconnected else 'unspecified',
               artifact=str(artifact), output=str(output), serial_log=str(args.serial_log.resolve()),
               serial_start=start, started=time.time(), status='running')
    run['reacquire_once'] = args.reacquire_once
    run['restore_pll'] = args.restore_pll
    run['restore_gain'] = args.restore_gain
    if args.action == 'receive':
        run['diagnostics_version'] = 1
        run['probe'] = getattr(args, 'probe', None)
    if args.action == 'receive':
        run['samples'] = args.samples or 30
    (output / 'checkpoint.json').write_text(json.dumps(run, indent=2) + '\n')
    base = guards(revision, sums)
    original_base = base
    module_dir = remote + '-modules'
    try:
        ssh(base + '''cat /build_info /proc/cmdline /proc/sys/kernel/panic /proc/mounts
cat /proc/mtd
ip -4 addr show dev br-lan
ip route show
cat /sys/class/net/lan1/carrier
dmesg
''' + idle_guards(), output / 'baseline.log')
        if args.registers:
            # Configuration/reset words only: no interrupt/FIFO/clear-on-read
            # registers and never a value argument to devmem. Addresses come
            # from en7581.dtsi, clk-en7523.c and the imported XG MAC layout.
            script = base + idle_guards() + 'test -c /dev/mem\n'
            for address, name in ((0x1fb00070, 'SCU_WAN_CONF'),
                                  (0x1fb0082c, 'SCU_CLK_CFG'),
                                  (0x1fb00830, 'SCU_RESET2'),
                                  (0x1fb00834, 'SCU_RESET1'),
                                  (0x1fb0092c, 'SCU_RESET_ACCESS_CHECK'),
                                  (0x1fb65000, 'XG_MAC_RESET'),
                                  (0x1fb65004, 'XG_MBI_MPI_STOP')):
                script += f"echo {name}\nbusybox devmem {address:#x} 32\n"
            ssh(script, output / 'registers.log')
        if update:
            after, active_sums, updates, payload = update
            stage = base + idle_guards() + f'''umask 077
test ! -e {module_dir}
mkdir -m 700 {module_dir}
mkdir {module_dir}/old {module_dir}/new
tar -xf - -C {module_dir}/new
'''
            # Verify all new bytes before replacing any installed RAM module.
            for path, data in updates.items():
                stage += f"echo '{hashlib.sha256(data).hexdigest()}  {module_dir}/new/{Path(path).name}' | sha256sum -c\n"
            for path in updates:
                name = Path(path).name
                stage += f'cp {path} {module_dir}/old/{name}\ncp {module_dir}/new/{name} {path}\n'
            ssh(stage, output / 'module-stage.log', payload)
            base = guards(revision, active_sums)
            run.update(module_source_revision=after, module_artifact=str(args.modules_from.resolve()),
                       module_backup=module_dir, updated_modules=list(updates))
        if args.action == 'resources':
            ssh(base + idle_guards() + resource_probe(), output / 'resources.log')
        if args.action in ('controller', 'stack', 'receive'):
            # All writes below are to verified RAM mounts or the runtime sysctl.
            stage = base + idle_guards() + f'''umask 077
 test ! -e {remote}
 test ! -e {FIRMWARE}/A60993.elf.pm
 test ! -e {FIRMWARE}/A60993.elf.dm
mkdir -m 700 {remote}
tar -xf - -C {remote}
cd {remote}
sha256sum -c sha256sums
mkdir -p {FIRMWARE}
cp A60993.elf.pm A60993.elf.dm {FIRMWARE}/
chmod 600 {FIRMWARE}/A60993.elf.pm {FIRMWARE}/A60993.elf.dm
printf '0\n' > /proc/sys/kernel/panic
 test "$(cat /proc/sys/kernel/panic)" = 0
echo 'RAM inputs verified; runtime kernel.panic=0 confirmed.'
'''
            ssh(stage, output / 'stage.log', args.inputs.read_bytes())
            run['ram_inputs'] = remote
            ssh(base + idle_guards() + f'''test "$(cat /proc/sys/kernel/panic)" = 0
q1000k-pon-bench {args.action} {remote}/xgspon-calibration.bin {fiber_flag}{recovery_flag}{sample_flag}
''', output / 'attempt.log', timeout=120 + 2 * (args.samples or 30))
        run['status'] = 'passed'
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
        run['status'] = 'failed'
        run['error'] = str(error)
    finally:
        # Read-only evidence after success or failure. Never force recovery.
        try:
            ssh(base + '''cat /proc/sys/kernel/panic
ip -4 addr show dev br-lan
cat /sys/class/net/lan1/carrier /sys/class/net/ponraw/flags
dmesg
''' + idle_guards(), output / 'postflight.log', timeout=25)
            run['postflight'] = 'passed'
        except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
            run['postflight'] = str(error)
            run['status'] = 'failed'
        if run['postflight'] == 'passed' and 'ram_inputs' in run:
            # Remove only this run's files, after proving their ownership/hashes.
            clean = base + idle_guards() + f'cd {remote}\nsha256sum -c sha256sums\n'
            for name in ('A60993.elf.pm', 'A60993.elf.dm'):
                clean += f"echo '{INPUTS[name][1]}  {FIRMWARE}/{name}' | sha256sum -c\n"
            clean += f'rm {FIRMWARE}/A60993.elf.pm {FIRMWARE}/A60993.elf.dm\n'
            clean += 'rm ' + ' '.join(INPUTS) + f' sha256sums\ncd /tmp\nrmdir {remote}\n'
            try:
                ssh(clean, output / 'cleanup.log')
                run['input_cleanup'] = 'passed'
            except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
                run['input_cleanup'] = str(error)
                run['status'] = 'failed'
        if run['postflight'] == 'passed' and 'module_backup' in run:
            try:
                restore = base + idle_guards()
                old = {p: h for h, p in (line.split() for line in sums.splitlines())}
                for path in run['updated_modules']:
                    restore += f"echo '{old[path]}  {module_dir}/old/{Path(path).name}' | sha256sum -c\n"
                for path in run['updated_modules']:
                    restore += f'cp {module_dir}/old/{Path(path).name} {path}\n'
                restore += original_base
                for path in run['updated_modules']:
                    restore += f'rm {module_dir}/old/{Path(path).name} {module_dir}/new/{Path(path).name}\n'
                restore += f'rmdir {module_dir}/old {module_dir}/new {module_dir}\n'
                ssh(restore, output / 'module-restore.log')
                run['module_restore'] = 'passed'
            except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
                run['module_restore'] = str(error)
                run['status'] = 'failed'
        serial.seek(start)
        (output / 'serial.log').write_bytes(serial.read())
        serial.close()
        run['finished'] = time.time()
        (output / 'checkpoint.json').write_text(json.dumps(run, indent=2) + '\n')
    print(json.dumps(run, indent=2))
    return 0 if run['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
