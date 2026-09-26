#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Inspect both normal PON FITs, actual root filesystems and upgrade metadata."""
import argparse
import gzip
import hashlib
import json
import lzma
from pathlib import Path
import posixpath
import re
import stat
import struct
import subprocess
import tempfile
import zlib

from check_pon_bench_image import cpio
from pon_image_format import fdt, u32, elf_defined_symbols

REPO = Path(__file__).resolve().parents[2]
MODULES = ('q1000k-pon-control', 'airoha_ecnt_hook', 'airoha_ecnt_scu', 'airoha_ecnt_pon_phy',
           'airoha_ecnt_xpon', 'phy_10g', 'xpon_10g', 'xpon', 'omci')


def fit_payloads(blob):
    fit, size = fdt(blob)
    payloads = {}
    for path, props in fit.items():
        if not path.startswith('/images/') or path.count('/') != 2:
            continue
        if 'data' in props:
            data = props['data']
        else:
            start = u32(props['data-position']) if 'data-position' in props else ((size + 3) & ~3) + u32(props['data-offset'])
            length = u32(props['data-size'])
            assert start + length <= len(blob)
            data = blob[start:start + length]
        hashes = [v for p, v in fit.items() if p.startswith(path + '/hash')]
        assert hashes, path
        for entry in hashes:
            algo = entry['algo'].rstrip(b'\0').decode()
            digest = struct.pack('>I', zlib.crc32(data)) if algo == 'crc32' else hashlib.new(algo, data).digest()
            assert digest == entry['value'], (path, algo)
        payloads[path.rsplit('/', 1)[1]] = (props, data)
    config = fit['/configurations/' + fit['/configurations']['default'].rstrip(b'\0').decode()]
    return payloads, config


def check_dt(tree):
    dt, _ = fdt(tree)
    assert b'quantum,q1000k-ubi' in dt['/']['compatible'].split(b'\0')
    assert 'quantum,xgspon-service' in dt['/']
    assert not any('quantum,xgspon-' + mode in dt['/'] for mode in ('bench', 'activation-bench'))
    rootdisk = dt['/chosen']['rootdisk']
    root = next(v for v in dt.values() if v.get('phandle') == rootdisk)
    assert root.get('volname') == b'fit\0', root
    nand = '/soc/spi@1fa10000'
    assert dt[nand]['status'] == b'okay\0'
    assert dt[nand + '/nand@0'].get('status', b'okay\0') == b'okay\0'
    assert 'read-only' not in dt[nand + '/nand@0']
    assert any('/partitions/' in p and v.get('label') == b'ubi\0' for p, v in dt.items())
    assert dt['/soc/pcs@1fa08000']['status'] == b'disabled\0'
    for path in ('/soc/phy@1faf0000', '/soc/pon@1fb64000'):
        assert dt[path]['status'] == b'okay\0'
    controller, = [v for v in dt.values() if v.get('compatible') == b'quantum,q1000k-pon-control\0']
    assert 'quantum,activation-bench' not in controller and 'quantum,tx-inhibit' not in controller
    assert struct.unpack('>III', controller['tx-disable-gpios'])[1:] == (38, 0)
    assert any(v.get('pins') == b'gpio38\0' and 'output-high' in v for v in dt.values())
    assert any(v.get('groups') == b'pon-sw-tx\0' for v in dt.values())
    lower = dt['/soc/ethernet@1fb50000/ethernet@2']
    assert lower['status'] == b'okay\0' and 'airoha,pon-port' in lower
    assert lower['phy-mode'] == b'internal\0' and 'pcs-handle' not in lower
    assert lower['openwrt,netdev-name'] == b'ponraw\0'
    assert u32(dt['/soc/ethernet@1fb50000/ethernet@2/fixed-link']['speed']) == 10000
    return hashlib.sha256(tree).hexdigest()


def ram_records(kernel):
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
    assert len(archives) == 1
    return archives[0]


def squash_records(data, workspace):
    image = workspace / 'rootfs.squashfs'
    image.write_bytes(data)
    root = workspace / 'root'
    unpacker = REPO / 'staging_dir/host/bin/unsquashfs4'
    # Inspect the only static device inode without requiring host mknod/root.
    listing = subprocess.check_output([unpacker, '-lln', image, 'dev/console'], text=True)
    assert re.search(r'^crw-------\s+0/0\s+5,\s+1\s.+/dev/console$', listing, re.M), listing
    subprocess.run([unpacker, '-no-progress', '-d', root, '-excludes', image, 'dev/console'],
                   check=True, capture_output=True)
    records = {}
    for p in root.rglob('*'):
        mode = p.lstat().st_mode
        data = str(p.readlink()).encode() if p.is_symlink() else p.read_bytes() if p.is_file() else b''
        records[p.relative_to(root).as_posix()] = (mode, data)
    return records


def check_root(records, revision, snapshot):
    def read(name, depth=0):
        assert depth < 16
        mode, data = records[name]
        if stat.S_ISLNK(mode):
            target = data.rstrip(b'\0').decode()
            target = posixpath.normpath(posixpath.join(posixpath.dirname(name), target)).lstrip('/')
            assert not target.startswith('../')
            return read(target, depth + 1)
        assert stat.S_ISREG(mode), name
        return data

    assert ('Revision: ' + revision).encode() in read('build_info')
    assert ('Working-tree snapshot: ' + snapshot).encode() in read('build_info')
    pairs = {
        'etc/uci-defaults/10-xgspon-config': 'network/utils/q1000k-xgspon/files/migrate-config',
        'etc/init.d/q1000k-xgspon': 'network/utils/q1000k-xgspon-service/files/legacy-init',
        'etc/init.d/q1000k-passthrough': 'network/utils/q1000k-passthrough/files/legacy-init',
        'usr/libexec/q1000k-xgspon-run': 'network/utils/q1000k-xgspon-service/files/run',
        'etc/init.d/xgspon': 'network/utils/q1000k-xgspon-service/files/init',
        'etc/uci-defaults/90-q1000k-xgspon-wan': 'network/utils/q1000k-xgspon-wan/files/defaults',
        'lib/q1000k-xgspon/common.sh': 'network/utils/q1000k-xgspon/files/common.sh',
        'lib/upgrade/keep.d/q1000k-xgspon': 'network/utils/q1000k-xgspon/files/keep',
        'usr/libexec/q1000k-passthrough-ipv4': 'network/utils/q1000k-passthrough/files/ipv4',
        'usr/libexec/q1000k-ipv6-transition': 'network/utils/q1000k-passthrough/files/ipv6-transition',
        'usr/libexec/q1000k-passthrough-watch-config': 'network/utils/q1000k-passthrough/files/watch-config',
        'etc/init.d/xgspon-passthrough': 'network/utils/q1000k-passthrough/files/init',
        'etc/hotplug.d/iface/95-q1000k-passthrough': 'network/utils/q1000k-passthrough/files/hotplug',
    }
    for name, source in pairs.items():
        assert read(name) == (REPO / 'package' / source).read_bytes(), name
    acl_path = 'usr/share/rpcd/acl.d/luci-app-econet-xpon.json'
    assert json.loads(read(acl_path)) == json.loads((REPO / 'package/luci-app-econet-xpon/root' / acl_path).read_bytes())
    assert read('etc/board.d/02_network') == (REPO /
        'target/linux/airoha/an7581/base-files/etc/board.d/02_network').read_bytes()
    assert 'etc/rc.d/S95q1000k-xgspon' not in records
    assert 'etc/rc.d/S96q1000k-passthrough' not in records
    assert 'etc/rc.d/S95xgspon' in records
    assert 'etc/rc.d/S96xgspon-passthrough' in records
    for path in ('usr/sbin/omci', 'usr/sbin/xgspon', 'usr/sbin/pon-factory', 'usr/sbin/xgspon-ipv6-transition', 'usr/sbin/q1000k-omci', 'usr/sbin/q1000k-pon-factory', 'usr/sbin/q1000k-xgspon',
                 'sbin/ip', 'bin/ubus', 'sbin/ifdown', 'usr/sbin/fitblk', 'usr/sbin/odhcp6c',
                 'usr/sbin/odhcpd', 'usr/sbin/uhttpd'):
        assert read(path), path
    assert 'usr/sbin/q1000k-pon-bench' not in records
    assert 'usr/sbin/q1000k-pon-validate' not in records
    assert 'etc/q1000k-private-autostart' not in records
    assert 'etc/uci-defaults/zz-q1000k-private-autostart' not in records
    assert b'192.168.0.1' in read('etc/board.d/99-lan-ip')
    hashes = {}
    for name in MODULES:
        path, = [p for p in records if p.startswith('lib/modules/') and p.endswith('/' + name + '.ko')]
        data = read(path)
        assert {'init_module', 'cleanup_module'} <= elf_defined_symbols(data), name
        hashes[name] = hashlib.sha256(data).hexdigest()
    for p in records:
        if p.startswith('etc/modules.d/') or p.startswith('etc/modules-boot.d/'):
            assert not any(name.replace('-', '_').encode() in read(p).replace(b'-', b'_') for name in MODULES), p
    for kind, version in (('settings', 7), ('status', 4)):
        source = (REPO / f'package/luci-app-econet-xpon/htdocs/luci-static/resources/view/econet-xpon/{kind}.js').read_bytes()
        minimized = subprocess.check_output([REPO / 'staging_dir/hostpkg/bin/jsmin'], input=source)
        assert read(f'www/luci-static/resources/view/econet-xpon/{kind}-v{version}.js') in (source, minimized)
    assert read('usr/libexec/q1000k-xgspon-watch-config') == (
        REPO / 'package/network/utils/q1000k-xgspon-service/files/watch-config').read_bytes()
    config = read('etc/config/xgspon')
    assert stat.S_IMODE(records['etc/config/xgspon'][0]) == 0o600
    assert config == (REPO / 'package/network/utils/q1000k-xgspon/files/xgspon.config').read_bytes()
    assert b"option enabled '1'" in config
    assert b"option enabled '0'" not in config
    assert 'lib/firmware/airoha/q1000k/xgspon-calibration.bin' not in records
    for name, digest in (
        ('A60993.elf.pm', '5a8a4bbae5f70c1e615ba0aa1c2a1dce654611d3205d2fa983bf41e6cdadb4a1'),
        ('A60993.elf.dm', '21618dc3694a1e6f6b28c7da7141964dea1d6e57f2d2956bbe72a780ca6166a4')):
        assert hashlib.sha256(read('lib/firmware/airoha/q1000k/' + name)).hexdigest() == digest
    assert not any(b'BGW320' in data or b'att.net' in data for mode, data in records.values() if stat.S_ISREG(mode))
    assert b'pi_ip="192.168.0.1"' in read('lib/preinit/00_preinit.conf')
    assert b'q1000k_wan' not in read('usr/libexec/q1000k-xgspon-run')
    assert b'q1000k_wan' not in read('etc/uci-defaults/90-q1000k-xgspon-wan')
    assert b'quantum,xgspon-service' in read('lib/q1000k-xgspon/common.sh')
    return hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--snapshot', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('images', type=Path, nargs=2)
    args = parser.parse_args()
    result = {}
    for image in args.images:
        blob = image.read_bytes()
        assert len(blob) <= 256 * 1024 * 1024
        payloads, config = fit_payloads(blob)
        dt_props, tree = payloads[config['fdt'].rstrip(b'\0').decode()]
        dt_hash = check_dt(tree)
        props, kernel = payloads[config['kernel'].rstrip(b'\0').decode()]
        assert props['type'] == b'kernel\0' and props['arch'] == b'arm64\0'
        assert u32(props['load']) == 0x80200000
        kind = 'initramfs' if '-initramfs-recovery.itb' in image.name else 'sysupgrade'
        with tempfile.TemporaryDirectory(prefix='inspect-', dir=args.output.parent) as temporary:
            workspace = Path(temporary)
            if kind == 'initramfs':
                assert props['compression'] == b'lzma\0'
                records = ram_records(kernel)
            else:
                assert props['compression'] == b'gzip\0' and gzip.decompress(kernel)
                name, = [n.decode() for n in config['loadables'].split(b'\0') if n.startswith(b'rootfs')]
                root_props, data = payloads[name]
                assert data[:4] == b'hsqs'
                records = squash_records(data, workspace)
                metadata = workspace / 'metadata.json'
                subprocess.run([REPO / 'staging_dir/host/bin/fwtool', '-i', metadata, image], check=True)
                metadata = json.loads(metadata.read_text())
                assert 'quantum,q1000k-ubi' in metadata['supported_devices']
            hashes = check_root(records, args.revision, args.snapshot)
        result[kind] = dict(image=image.name, bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest(),
                            dtb_sha256=dt_hash, module_sha256=hashes, fit_hashes_verified=True,
                            rootfs_verified=True, private_inputs_absent=True)
    assert set(result) == {'initramfs', 'sysupgrade'}
    assert result['initramfs']['dtb_sha256'] == result['sysupgrade']['dtb_sha256']
    assert result['initramfs']['module_sha256'] == result['sysupgrade']['module_sha256']
    args.output.write_text(json.dumps(dict(images=result, same_pon_runtime=True, device_access=False), indent=2) + '\n')
    print('Both FITs, UBI/PON DT, packaged runtime, module parity, shared firmware, absent private configuration and sysupgrade metadata verified.')


if __name__ == '__main__':
    main()
