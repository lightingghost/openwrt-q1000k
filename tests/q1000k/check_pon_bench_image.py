#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the exact FIT, embedded initramfs and DT of a local RAM bench image."""
import argparse
import hashlib
import json
import lzma
from pathlib import Path
import posixpath
import re
import stat
import struct
import zlib
from pon_image_format import elf_defined_symbols, fdt, u32


def cpio(data, start):
    """Read a newc archive into memory; never extract untrusted paths to disk."""
    offset, records = start, {}
    while True:
        assert data[offset:offset + 6] == b'070701'
        fields = [int(data[i:i + 8], 16) for i in range(offset + 6, offset + 110, 8)]
        mode, length, namesize = fields[1], fields[6], fields[11]
        assert 0 < namesize < 4096
        name_end = offset + 110 + namesize
        assert data[name_end - 1] == 0
        name = data[offset + 110:name_end - 1].decode('utf-8')
        offset = start + ((name_end - start + 3) & ~3)
        assert offset + length <= len(data)
        payload = data[offset:offset + length]
        offset = start + ((offset + length - start + 3) & ~3)
        if name == 'TRAILER!!!':
            return records, offset
        name = name.removeprefix('./')
        assert name not in records, name
        records[name] = (mode, payload)


def inspect(image, revision):
    repo = Path(__file__).resolve().parents[2]
    blob = image.read_bytes()
    fit, size = fdt(blob)
    payloads, checked = {}, []
    for path, props in fit.items():
        if not path.startswith('/images/') or path.count('/') != 2:
            continue
        if 'data' in props:
            payload = props['data']
        else:
            length = u32(props['data-size'])
            position = u32(props['data-position']) if 'data-position' in props else (
                (size + 3) & ~3) + u32(props['data-offset'])
            assert position + length <= len(blob)
            payload = blob[position:position + length]
        count = 0
        for child, values in fit.items():
            if child.startswith(path + '/hash'):
                algo = values['algo'].rstrip(b'\0').decode('ascii')
                digest = struct.pack('>I', zlib.crc32(payload)) if algo == 'crc32' else hashlib.new(algo, payload).digest()
                assert digest == values['value'], (path, algo)
                count += 1
        assert count
        payloads[path.rsplit('/', 1)[1]] = (props, payload)
        checked.append(path)
    default = fit['/configurations']['default'].rstrip(b'\0').decode()
    config = fit['/configurations/' + default]
    kernel_props, kernel = payloads[config['kernel'].rstrip(b'\0').decode()]
    dt_props, tree = payloads[config['fdt'].rstrip(b'\0').decode()]
    assert 'ramdisk' not in config  # This profile embeds newc in the kernel.
    assert kernel_props['type'] == b'kernel\0' and kernel_props['arch'] == b'arm64\0'
    assert kernel_props['compression'] == b'lzma\0'
    assert u32(kernel_props['load']) == 0x80200000
    assert dt_props['type'] == b'flat_dt\0'
    dt, _ = fdt(tree)
    assert b'quantum,q1000k-ubi' in dt['/']['compatible'].split(b'\0')
    assert 'quantum,xgspon-bench' in dt['/']
    assert 'rootdisk' not in dt['/chosen']
    assert dt['/chosen']['bootargs'] == b'console=ttyS0,115200 earlycon\0'
    nand = '/soc/spi@1fa10000'
    assert dt[nand]['status'] == b'disabled\0'
    assert dt[nand + '/nand@0']['status'] == b'disabled\0'
    assert not any(p.startswith(nand + '/nand@0/partitions') for p in dt)
    controllers = [p for p, v in dt.items() if b'quantum,q1000k-pon-control' in v.get('compatible', b'').split(b'\0')]
    assert len(controllers) == 1
    assert 'quantum,tx-inhibit' in dt[controllers[0]]
    assert dt['/soc/pcs@1fa08000']['status'] == b'disabled\0'
    for path in ('/soc/phy@1faf0000', '/soc/pon@1fb64000'):
        assert dt[path]['status'] == b'okay\0'
    lower = dt['/soc/ethernet@1fb50000/ethernet@2']
    assert lower['status'] == b'okay\0' and 'airoha,pon-port' in lower
    assert lower['phy-mode'] == b'internal\0' and 'pcs-handle' not in lower
    assert lower['openwrt,netdev-name'] == b'ponraw\0'
    assert u32(dt['/soc/ethernet@1fb50000/ethernet@2/fixed-link']['speed']) == 10000

    expanded = lzma.decompress(kernel)
    archives, offset = [], 0
    while (offset := expanded.find(b'070701', offset)) >= 0:
        try:
            records, end = cpio(expanded, offset)
            if 'build_info' in records:
                archives.append(records)
            offset = end
            continue
        except (AssertionError, ValueError, IndexError, UnicodeError):
            pass
        offset += 6
    assert len(archives) == 1, len(archives)
    records = archives[0]

    def read(name, depth=0):
        assert depth < 16, name
        mode, data = records[name]
        if stat.S_ISLNK(mode):
            # Linux gen_init_cpio includes the symlink target's terminal NUL.
            data = data.removesuffix(b'\0')
            assert b'\0' not in data, name
            target = posixpath.normpath(posixpath.join(posixpath.dirname(name), data.decode()))
            assert not target.startswith('../'), target
            return read(target.lstrip('/'), depth + 1)
        assert stat.S_ISREG(mode), name
        return data

    assert read('build_info').decode().splitlines()[3] == 'Revision: ' + revision
    assert b'(bench)' in read('build_info')
    preinit = read('lib/preinit/00_preinit.conf')
    assert b'pi_ip="192.168.255.1"\n' in preinit
    assert b'pi_broadcast="192.168.255.255"\n' in preinit
    assert b'192.168.1.' not in preinit
    assert b'json_add_string ipaddr "192.168.255.1"' in read('etc/board.d/99-lan-ip')
    for source, dest in (
        ('package/network/utils/q1000k-xgspon-bench/files/defaults', 'etc/uci-defaults/99-q1000k-xgspon-bench'),
        ('package/network/utils/q1000k-xgspon-bench/files/bench', 'usr/sbin/q1000k-pon-bench'),
        ('package/network/utils/q1000k-xgspon/files/q1000k-xgspon.config', 'etc/config/q1000k-xgspon')):
        assert read(dest) == (repo / source).read_bytes(), dest
    for name in ('usr/sbin/q1000k-omci', 'usr/sbin/q1000k-pon-factory', 'usr/sbin/q1000k-xgspon'):
        assert read(name), name
    modules = ('q1000k-pon-control', 'airoha_ecnt_hook', 'airoha_ecnt_scu', 'airoha_ecnt_pon_phy',
               'airoha_ecnt_xpon', 'phy_10g', 'xpon_10g', 'xpon', 'omci')
    for name in modules:
        matches = [p for p in records if p.startswith('lib/modules/') and p.endswith('/' + name + '.ko')]
        assert len(matches) == 1, name
        assert read(matches[0]).startswith(b'\x7fELF')
        symbols = elf_defined_symbols(read(matches[0]))
        # Every bench module owns initialization, including the hook lists.
        # A library with neither callback can unload but is not initialized.
        assert {'init_module', 'cleanup_module'} <= symbols, (name, 'module lacks init/exit lifecycle')
    forbidden = {n.replace('-', '_') for n in modules}
    for name in records:
        if name.startswith(('etc/modules.d/', 'etc/modules-boot.d/')):
            for line in read(name).decode().splitlines():
                if line.strip() and not line.lstrip().startswith('#'):
                    assert line.split()[0].replace('-', '_') not in forbidden, name
    assert not any(p.startswith('lib/firmware/airoha/q1000k/') for p in records)
    return dict(revision=revision, file=image.name, bytes=len(blob),
                sha256=hashlib.sha256(blob).hexdigest(), verified_fit_images=checked,
                kernel_uncompressed_bytes=len(expanded), initramfs_entries=len(records),
                unloadable_pon_modules=list(modules),
                nand_disabled=True, tx_inhibited=True, management_ip='192.168.255.1',
                rootfs_checks='passed', device_access=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('revision')
    args = parser.parse_args()
    if not __debug__ or not re.fullmatch('[0-9a-f]{40}', args.revision):
        parser.error('Require a full lowercase commit and non-optimized Python')
    print(json.dumps(inspect(args.image, args.revision), indent=2))
