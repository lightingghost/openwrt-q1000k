#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract this unit's OEM reference from its NAND backup, read-only.

No device access, bootloader commands, password extraction or OEM execution.
Outputs contain proprietary firmware; keep the new output directory private.
"""
import argparse
import gzip
import hashlib
import json
import lzma
from pathlib import Path
import struct
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tests/q1000k'))
from pon_image_format import fdt

NAND_SHA256 = '41f08f7e71c5c08835fd1f923a925e10bf56add1239b1e35ad35df6da8e76c50'
INPUTS = {
    'A60993.elf.pm': '5a8a4bbae5f70c1e615ba0aa1c2a1dce654611d3205d2fa983bf41e6cdadb4a1',
    'A60993.elf.dm': '21618dc3694a1e6f6b28c7da7141964dea1d6e57f2d2956bbe72a780ca6166a4',
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nand', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New private directory')
    args = parser.parse_args()
    source = args.nand.resolve(strict=True)
    digest = hashlib.sha256()
    with source.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
        if source.stat().st_size != 512 * 1024 * 1024 or digest.hexdigest() != NAND_SHA256:
            raise ValueError('This extractor is pinned to the audited Q1000K NAND layout and unit')
        stream.seek(0x602100)
        header = stream.read(8)
        magic, size = struct.unpack('>II', header)
        if magic != 0xd00dfeed or not 8 < size < 0x4000000 - 0x2100:
            raise ValueError('Invalid primary FIT bounds')
        fit = header + stream.read(size - 8)
        stream.seek(0x412000)
        calibration = stream.read(513)
    nodes, _ = fdt(fit)
    verified_hashes = {}
    for path, node in nodes.items():
        if '/hash@' in path:
            algorithm = node['algo'].rstrip(b'\0').decode()
            payload = nodes[path.rsplit('/', 1)[0]]['data']
            if algorithm not in ('sha1', 'sha256') or hashlib.new(algorithm, payload).digest() != node['value']:
                raise ValueError('FIT hash mismatch: ' + path)
            verified_hashes[path] = algorithm
    output = args.output.resolve()
    output.mkdir(mode=0o700)
    files = {}
    def save(name, data):
        path = output / name
        path.write_bytes(data)
        path.chmod(0o600)
        files[name] = dict(bytes=len(data), sha256=sha(data))
    save('primary-fit.itb', fit)
    for node, name in (('kernel', 'kernel.lzma'), ('fdt', 'oem.dtb'), ('filesystem', 'rootfs.squashfs')):
        save(name, nodes[f'/images/{node}@1']['data'])
    kernel = lzma.decompress(nodes['/images/kernel@1']['data'])
    save('oem-kernel.Image', kernel)
    start, end = kernel.index(b'IKCFG_ST') + 8, kernel.index(b'IKCFG_ED')
    config = gzip.decompress(kernel[start:end])
    save('oem-kernel.config', config)
    firmware = {}
    for name, expected in INPUTS.items():
        data = subprocess.check_output(['unsquashfs', '-cat', str(output/'rootfs.squashfs'), 'etc/lddla/'+name])
        firmware[name] = dict(bytes=len(data), sha256=sha(data), matches_bench=sha(data) == expected)
        if sha(data) != expected:
            raise ValueError('OEM controller firmware differs from current bench: ' + name)
    subprocess.run([sys.executable, str(REPO/'scripts/q1000k/oem-pon-reference.py'),
                    '--image', str(output/'rootfs.squashfs'), '--output', str(output/'pon-reference')], check=True)
    save('inittab.txt', subprocess.check_output(['unsquashfs', '-cat', str(output/'rootfs.squashfs'), 'etc/inittab']))
    record = dict(nand=str(source), nand_sha256=digest.hexdigest(), source_modified=False,
                  fit_offset='0x602100', verified_fit_hashes=verified_hashes, files=files,
                  controller_firmware=firmware,
                  dsd=dict(offset='0x412000', bytes=513, sha256=sha(calibration),
                           matches_bench=sha(calibration) == 'f2ec3b0de9683d113755d5d4df4fcafe8a4b47a43153ad0de45cbe9cd34c6e1c'),
                  boot_feasibility=dict(external_initrd=b'CONFIG_BLK_DEV_INITRD=y' in config,
                                        compressed_cpio_supported=any(b'CONFIG_RD_'+name+b'=y' in config for name in (b'GZIP', b'BZIP2', b'LZMA', b'XZ', b'LZO', b'LZ4')),
                                        kernel_load='0x80088000',
                                        root_strategy='Uncompressed external newc CPIO with custom /init and console nodes; bypass OEM restricted console',
                                        oem_ram_boot_tested=False, oem_root_shell_tested=False),
                  device_access=False, oem_code_executed=False)
    save('audit.json', (json.dumps(record, indent=2)+'\n').encode())
    print(output/'audit.json')


if __name__ == '__main__':
    main()
