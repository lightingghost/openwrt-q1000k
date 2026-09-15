#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Recreate the private RAM-test archive from saved verified inputs; no SSH."""
import argparse
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import tarfile

SPEC = importlib.util.spec_from_file_location('bench_run', Path(__file__).with_name('bench-run.py'))
RUN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN)


def prepare(source, output):
    files = {}
    for name, (size, digest) in RUN.INPUTS.items():
        path = source / name if name == 'xgspon-calibration.bin' else source / 'lib/firmware/airoha/q1000k' / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size != size:
            raise ValueError('Unexpected private input file: ' + name)
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError('Private input hash mismatch: ' + name)
        files[name] = data
    files['sha256sums'] = ''.join(f'{digest}  {name}\n' for name, (_, digest) in RUN.INPUTS.items()).encode()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w') as tar:
        for name, data in files.items():
            member = tarfile.TarInfo(name)
            member.size, member.mode = len(data), 0o600
            tar.addfile(member, io.BytesIO(data))
    # No overwrite, including symlinks; never print the archive's contents.
    with os.fdopen(os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as stream:
        stream.write(archive.getvalue())
    RUN.validate_inputs(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='Saved private-inputs directory')
    parser.add_argument('--output', type=Path, required=True, help='New private tar archive')
    args = parser.parse_args()
    prepare(args.source, args.output)
    print(f'Verified private RAM-test archive ready: {args.output} (mode 0600)')


if __name__ == '__main__':
    main()
