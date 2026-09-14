#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Inspect local experimental FIT images; never boot or execute their contents.

Usage: python3 tests/q1000k/check_pon_images.py ARTIFACT_DIRECTORY FULL_SOURCE_REVISION
Requires the built host unsquashfs4 in this OpenWrt checkout.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib
import re

repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=Path)
parser.add_argument('revision')
args = parser.parse_args()
if not __debug__:
    parser.error('Run without Python optimization so assertions remain enabled')
if not re.fullmatch('[0-9a-f]{40}', args.revision):
    parser.error('A full, lowercase 40-character source commit is required')
output, revision = args.directory, args.revision


def u32(data, offset=0):
    return struct.unpack_from('>I', data, offset)[0]


def fdt(data):
    magic, size, tokens, strings, _, version, _, _, strings_size, tokens_size = struct.unpack_from('>10I', data)
    assert magic == 0xd00dfeed and version == 17 and size <= len(data)
    assert tokens + tokens_size <= size and strings + strings_size <= size
    names = data[strings:strings + strings_size]
    end = tokens + tokens_size
    nodes, stack = {}, []
    while tokens < end:
        token = u32(data, tokens)
        tokens += 4
        if token == 1:
            stop = data.index(0, tokens, end)
            stack.append(data[tokens:stop].decode('ascii'))
            path = '/'.join(stack) or '/'
            assert path not in nodes
            nodes[path] = {}
            tokens = (stop + 4) & ~3
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, name = struct.unpack_from('>II', data, tokens)
            tokens += 8
            assert tokens + length <= end and name < len(names)
            key = names[name:names.index(0, name)].decode('ascii')
            path = '/'.join(stack) or '/'
            assert key not in nodes[path]
            nodes[path][key] = data[tokens:tokens + length]
            tokens = (tokens + length + 3) & ~3
        elif token == 9:
            assert not stack
            return nodes, size
        else:
            assert token == 4
    raise AssertionError('FDT lacks an end token')


reports, squash = [], None
for suffix in ('initramfs-recovery.itb', 'squashfs-sysupgrade.itb'):
    matches = list(output.glob('*-quantum_q1000k-ubi-' + suffix))
    assert len(matches) == 1, matches
    image = matches[0]
    blob = image.read_bytes()
    nodes, tree_size = fdt(blob)
    checked, disabled = [], None
    for name, properties in nodes.items():
        if not name.startswith('/images/') or name.count('/') != 2:
            continue
        if 'data' in properties:
            payload = properties['data']
        else:
            length = u32(properties['data-size'])
            position = u32(properties['data-position']) if 'data-position' in properties else (
                (tree_size + 3) & ~3) + u32(properties['data-offset'])
            assert position + length <= len(blob)
            payload = blob[position:position + length]
        hashes = 0
        for child, values in nodes.items():
            if child.startswith(name + '/hash'):
                algorithm = values['algo'].rstrip(b'\0').decode('ascii')
                digest = (struct.pack('>I', zlib.crc32(payload)) if algorithm == 'crc32'
                          else hashlib.new(algorithm, payload).digest())
                assert digest == values['value'], (image.name, name, algorithm)
                hashes += 1
        assert hashes >= 1
        if properties.get('type') == b'flat_dt\0':
            dt, _ = fdt(payload)
            assert b'quantum,q1000k-ubi' in dt['/']['compatible'].split(b'\0')
            disabled = []
            for path, props in dt.items():
                compatible = props.get('compatible', b'').split(b'\0')
                if any(c in compatible for c in (b'quantum,q1000k-pon-phy', b'quantum,q1000k-pon-mac',
                                                 b'airoha,an7581-pcs-pon')) or 'airoha,pon-port' in props:
                    assert props.get('status') == b'disabled\0', path
                    disabled.append(path)
            assert len(disabled) == 4, disabled
        if payload[:4] == b'hsqs':
            assert squash is None
            squash = payload
        checked.append(name)
    assert disabled is not None
    reports.append(dict(file=image.name, bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest(),
                        verified_fit_images=checked, disabled_resources=disabled))

assert squash is not None
with tempfile.TemporaryDirectory(prefix='q1000k-image-inspection.') as temporary:
    temporary = Path(temporary)
    filesystem = temporary / 'rootfs.squashfs'
    filesystem.write_bytes(squash)
    root = temporary / 'root'
    subprocess.run([str(repo / 'staging_dir/host/bin/unsquashfs4'), '-no-progress', '-d', str(root),
                    str(filesystem), 'build_info', 'etc', 'usr', 'lib', 'www'],
                   check=True, stdout=subprocess.DEVNULL)
    assert (root / 'build_info').read_text().splitlines()[3] == 'Revision: ' + revision
    config = repo / 'package/network/utils/q1000k-xgspon/files/q1000k-xgspon.config'
    assert (root / 'etc/config/q1000k-xgspon').read_bytes() == config.read_bytes()
    for name in ('usr/sbin/q1000k-xgspon', 'usr/sbin/q1000k-omci', 'usr/sbin/q1000k-pon-factory',
                 'usr/libexec/q1000k-xgspon-run', 'etc/init.d/q1000k-xgspon',
                 'etc/uci-defaults/90-q1000k-xgspon-wan',
                 'www/luci-static/resources/view/econet-xpon/status.js'):
        assert (root / name).is_file(), name
    modules = ('q1000k-pon-control', 'airoha_ecnt_hook', 'airoha_ecnt_scu', 'airoha_ecnt_pon_phy',
               'airoha_ecnt_xpon', 'phy_10g', 'xpon_10g', 'xpon', 'omci')
    for name in modules:
        assert len(list((root / 'lib/modules').glob('*/' + name + '.ko'))) == 1, name
    forbidden = {name.replace('-', '_') for name in modules}
    for folder in ('etc/modules.d', 'etc/modules-boot.d'):
        for path in (root / folder).glob('*'):
            for line in path.read_text().splitlines():
                if line.strip() and not line.lstrip().startswith('#'):
                    assert line.split()[0].replace('-', '_') not in forbidden, path

report = dict(revision=revision, images=reports, rootfs_checks='passed', device_access=False)
(output / 'inspection.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
