#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Save local OEM PON binaries and disassembly for comparison; never execute them."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

REPO = Path(__file__).resolve().parents[2]
FILES = ('lib/modules/5.4.55/en7572.ko', 'lib/modules/5.4.55/phy_10g.ko',
         'etc/init.d/xponconfig')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True, help='Local OEM squashfs file')
    parser.add_argument('--output', type=Path, required=True, help='New private directory')
    args = parser.parse_args()
    image = args.image.resolve(strict=True)
    objdump, = (REPO / 'staging_dir').glob('toolchain-aarch64*/bin/aarch64-openwrt-linux-musl-objdump')
    output = args.output.resolve()
    output.mkdir(mode=0o700)
    record = {'source': str(image), 'sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
              'objdump': str(objdump), 'files': {}, 'device_access': False,
              'oem_code_executed': False}
    for name in FILES:
        data = subprocess.check_output(['unsquashfs', '-cat', str(image), name])
        if name.endswith('.ko') and not data.startswith(b'\x7fELF'):
            raise ValueError('OEM module is not ELF: ' + name)
        target = output / Path(name).name
        target.write_bytes(data)
        target.chmod(0o600)
        record['files'][name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        if name.endswith('.ko'):
            with (output / (target.name + '.dis')).open('wb') as stream:
                subprocess.run([str(objdump), '-dr', str(target)], stdout=stream, check=True)
    (output / 'reference.json').write_text(json.dumps(record, indent=2) + '\n')
    print(output / 'reference.json')


if __name__ == '__main__':
    main()
