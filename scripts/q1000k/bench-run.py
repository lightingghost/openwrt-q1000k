#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Capture a verified Q1000K RAM bench run. Never boots or flashes a device."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import tarfile
import time

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
                '/usr/share/libubox/jshn.sh', '/usr/sbin/q1000k-omci'}
    expected_modules = {m.replace('_', '-') if m == 'q1000k_pon_control' else m for m in MODULES}
    modules = [p for p in names if re.fullmatch(r'/lib/modules/[0-9][0-9A-Za-z.+-]*/[-a-z0-9_]+\.ko', p)]
    if (len(names) != 13 or len(set(names)) != 13 or set(names) - set(modules) != expected
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['status', 'controller', 'stack'])
    parser.add_argument('--artifact', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path, help='New private capture directory')
    parser.add_argument('--serial-log', type=Path, default=Path('/tmp/serial_output.log'))
    parser.add_argument('--inputs', type=Path, help='Verified private tar; never copied to the repository')
    parser.add_argument('--fiber-disconnected', action='store_true')
    args = parser.parse_args()
    if args.action != 'status' and (not args.fiber_disconnected or args.inputs is None):
        parser.error('Tests require --fiber-disconnected and --inputs; status is read-only')
    artifact = args.artifact.resolve()
    revision = json.loads((artifact / 'selection.json').read_text())['revision']
    if not re.fullmatch('[0-9a-f]{40}', revision):
        raise ValueError('Invalid artifact revision')
    sums = runtime_manifest(artifact / 'runtime-sha256sums')
    if args.action != 'status':
        validate_inputs(args.inputs)
    output = args.output.resolve()
    remote = '/tmp/q1000k-bench-' + output.name
    if not re.fullmatch(r'/tmp/q1000k-bench-[-A-Za-z0-9_.]+', remote):
        raise ValueError('Use a simple output directory name')
    serial = args.serial_log.open('rb')  # Retain the descriptor across rotation.
    output.mkdir(mode=0o700)  # Never overwrite a prior capture.
    start = serial.seek(0, 2)
    run = dict(schema_version=1, action=args.action, host=HOST, revision=revision,
               artifact=str(artifact), output=str(output), serial_log=str(args.serial_log.resolve()),
               serial_start=start, started=time.time(), status='running')
    (output / 'checkpoint.json').write_text(json.dumps(run, indent=2) + '\n')
    base = guards(revision, sums)
    try:
        ssh(base + '''cat /build_info /proc/cmdline /proc/sys/kernel/panic /proc/mounts
cat /proc/mtd
ip -4 addr show dev br-lan
ip route show
cat /sys/class/net/lan1/carrier
dmesg
''' + idle_guards(), output / 'baseline.log')
        if args.action != 'status':
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
q1000k-pon-bench {args.action} {remote}/xgspon-calibration.bin --fiber-disconnected
''', output / 'attempt.log')
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
        serial.seek(start)
        (output / 'serial.log').write_bytes(serial.read())
        serial.close()
        run['finished'] = time.time()
        (output / 'checkpoint.json').write_text(json.dumps(run, indent=2) + '\n')
    print(json.dumps(run, indent=2))
    return 0 if run['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
