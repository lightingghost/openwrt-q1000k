#!/usr/bin/env python3
"""Extract and verify Q1000K optical firmware from the user's OEM SquashFS.

No vendor binaries are executed. No firmware or calibration is downloaded.
Output is an OpenWrt files/ overlay suitable for a local experimental build.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

FIRMWARE = {
    'A60993.elf.pm': (15232, '5a8a4bbae5f70c1e615ba0aa1c2a1dce654611d3205d2fa983bf41e6cdadb4a1'),
    'A60993.elf.dm': (56, '21618dc3694a1e6f6b28c7da7141964dea1d6e57f2d2956bbe72a780ca6166a4'),
}


def extract(squashfs, output):
    blobs = {}
    for name, (size, digest) in FIRMWARE.items():
        result = subprocess.run(['unsquashfs', '-cat', str(squashfs), 'etc/lddla/' + name],
                                check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        data = result.stdout
        if len(data) != size or hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f'{name}: does not match the QKX001-06.00.44.00 baseline')
        blobs[name] = data
    # Verify every input before creating outputs. Publish a complete new overlay
    # directory with rename; never overwrite an existing user overlay.
    output = Path(output).absolute()
    if output.exists() or output.is_symlink():
        raise ValueError('output directory already exists; choose a new directory')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.q1000k-pon-', dir=output.parent) as tmp:
        root = Path(tmp) / 'overlay'
        dest = root / 'lib/firmware/airoha/q1000k'
        dest.mkdir(parents=True)
        for name, data in blobs.items():
            path = dest / name
            path.write_bytes(data)
            path.chmod(0o644)
        # rename won't replace a populated directory. Refuse empty directories
        # too, so an existing files/ tree is never silently taken over.
        if output.exists() or output.is_symlink():
            raise ValueError('output directory appeared during extraction')
        os.rename(root, output)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--squashfs', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        output = extract(args.squashfs, args.output_dir)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f'Firmware extraction failed: {exc}\n')
    print(f'Verified optical firmware overlay: {output}')


if __name__ == '__main__':
    main()
