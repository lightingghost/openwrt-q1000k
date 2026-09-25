#!/usr/bin/env python3
"""Build pinned DHCP software into bench artifacts for isolated host tests.

No package installation, network access, target configuration or source edits.
odhcpd/odhcp6c use their standard no-ubus build and explicit fixture config.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT/'build_dir/target-aarch64_cortex-a53_musl'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    if not out.is_relative_to(ROOT.parent/'build-artifacts'):
        parser.error('output must be in workspace build-artifacts')
    out.mkdir(parents=True, exist_ok=True)
    host = ROOT/'staging_dir/host'
    def one(pattern):
        paths = list(TARGET.glob(pattern))
        if len(paths) != 1: raise RuntimeError(f'Prepare exactly one {pattern}: {paths}')
        return paths[0]
    records = []
    def run(name, argv, cwd=None):
        with (out/(name+'.log')).open('w') as log:
            p = subprocess.run(list(map(str, argv)), cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
        records.append({'step': name, 'argv': list(map(str, argv)), 'returncode': p.returncode})
        (out/'build.json').write_text(json.dumps(records, indent=2)+'\n')
        if p.returncode: raise RuntimeError(f'{name} failed; see {out/(name+".log")}')
    nl = one('libnl-tiny-*/CMakeLists.txt').parent
    uci = one('uci-*/libuci.c').parent
    server = one('odhcpd-ipv6only/odhcpd-*/CMakeLists.txt').parent
    client = one('odhcp6c-*/CMakeLists.txt').parent
    run('nl-configure', ['cmake','-S',nl,'-B',out/'nl'])
    run('nl-build', ['cmake','--build',out/'nl','-j','4'])
    run('uci-build', ['gcc','-shared','-fPIC','-O2','-I'+str(uci),'-I'+str(host/'include'),
        *[uci/p for p in ('libuci.c','file.c','util.c','delta.c','parse.c','blob.c')],
        '-L'+str(host/'lib'),'-Wl,-rpath,'+str(host/'lib'),'-lubox','-ldl','-o',out/'libuci.so'])
    run('server-configure', ['cmake','-S',server,'-B',out/'server','-DUBUS=OFF','-DDHCPV4_SUPPORT=OFF',
        '-Duci_include_dir='+str(uci),'-Dlibuci='+str(out/'libuci.so'),
        '-Dubox_include_dir='+str(host/'include'),'-Dlibubox='+str(host/'lib/libubox.so'),
        '-Dlibnl-tiny_include_dir='+str(nl/'include'),'-Dlibnl='+str(out/'nl/libnl-tiny.so'),
        '-Djson_include_dir='+str(host/'include/json-c'),'-Dlibjson='+str(host/'lib/libjson-c.a')])
    run('server-build', ['cmake','--build',out/'server','-j','4'])
    run('client-configure', ['cmake','-S',client,'-B',out/'client','-DUBUS=OFF',
        '-Dubox_include_dir='+str(host/'include'),'-Dlibubox='+str(host/'lib/libubox.so')])
    run('client-build', ['cmake','--build',out/'client','-j','4'])
    dns = one('dnsmasq-nodhcpv6/dnsmasq-*/Makefile').parent
    if not (out/'dnsmasq').exists():
        shutil.copytree(dns, out/'dnsmasq', ignore=shutil.ignore_patterns('*.o','dnsmasq','ipkg-*','.pkgdir'))
    run('dnsmasq-build', ['make','-j4','CC=gcc',
        'COPTS=-DNO_UBUS -DNO_IDN -DNO_LUA -DNO_DBUS -DNO_DHCP6 -DNO_CONNTRACK -DNO_NFTSET -DNO_IPSET',
        'CFLAGS=-O2','LDFLAGS='], cwd=out/'dnsmasq')
    print(out)


if __name__ == '__main__':
    main()
