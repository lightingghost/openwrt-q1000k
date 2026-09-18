#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Collect all five Q1000K validation stages with one pinned RAM image.

Python 3 and OpenSSH only. No flash/boot actions. --identity explicitly selects
normal optical TX activation after RX tests. Private inputs never enter the kit.
"""
import argparse
import fcntl
import hashlib
import io
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import select
import shlex
import signal
import subprocess
import sys
import tarfile
import tempfile
import time

PIN = None
TIMING_SLEEP = None
HOST = '192.168.255.1'
IMAGE = 'openwrt-airoha-an7581-quantum_q1000k-xgspon-activation-initramfs-bench.itb'
MODULES = 'q1000k_pon_control airoha_ecnt_hook airoha_ecnt_scu airoha_ecnt_pon_phy airoha_ecnt_xpon phy_10g xpon omci xpon_10g'.split()
INPUTS = {
    'xgspon-calibration.bin': (513, 'f2ec3b0de9683d113755d5d4df4fcafe8a4b47a43153ad0de45cbe9cd34c6e1c'),
    'A60993.elf.pm': (15232, '5a8a4bbae5f70c1e615ba0aa1c2a1dce654611d3205d2fa983bf41e6cdadb4a1'),
    'A60993.elf.dm': (56, '21618dc3694a1e6f6b28c7da7141964dea1d6e57f2d2956bbe72a780ca6166a4'),
}
STAGE = '/tmp/q1000k-activation-inputs'
FIRMWARE = '/lib/firmware/airoha/q1000k'
SSH = ['ssh', '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null',
       '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', '-o', 'ServerAliveInterval=5',
       '-o', 'ServerAliveCountMax=2', 'root@' + HOST]
KEYS = 'serial vendor_id equipment_id hardware_version sync_circuit_pack software_version_a software_version_b active_bank committed_bank registration_id logical_onu_id logical_password wan_mac omci_version mib_profile fix_vlans'.split()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def validate_identity(value):
    if not isinstance(value, dict) or set(value) - set(KEYS):
        raise ValueError('Unknown identity fields; use the supplied template')
    for key, text in value.items():
        if not isinstance(text, str) or any(ord(c) < 32 or ord(c) > 126 for c in text):
            raise ValueError('Identity values must be printable ASCII strings: ' + key)
        if not text:
            continue
        pattern = {'serial': r'[A-Za-z0-9]{4}[0-9A-Fa-f]{8}', 'vendor_id': r'[A-Za-z0-9]{4}',
                   'wan_mac': r'[0-9a-fA-F][02468aAcCeE](:[0-9a-fA-F]{2}){5}',
                   'registration_id': r'(?:[0-9a-fA-F]{2}){1,36}',
                   'mib_profile': 'native-pptp'}.get(key)
        if key in ('sync_circuit_pack', 'active_bank', 'committed_bank', 'fix_vlans'):
            pattern = '[01]'
        maximum = {'equipment_id': 20, 'logical_onu_id': 24, 'logical_password': 12}.get(key, 14)
        if (pattern and not re.fullmatch(pattern, text)) or (not pattern and len(text) > maximum):
            raise ValueError('Invalid identity field: ' + key)
    if any(not value.get(k) for k in ('serial', 'wan_mac')):
        raise ValueError('Explicit subscriber serial and wan_mac are required for TX')
    if value['wan_mac'] == '00:00:00:00:00:00':
        raise ValueError('WAN MAC must be nonzero')
    # The wire format needs 36 bytes, not a separately supplied credential on
    # every ISP. In particular the 8311 AT&T BGW320 recipe sets no reg_id_hex.
    # Materialize the zero default for the already shipped launcher, which
    # requires an explicit field. Preserve a caller's optional nonempty value.
    return dict(value, registration_id=value.get('registration_id') or '00' * 36)


def private_inputs(path, identity):
    files = {}
    with tarfile.open(path, 'r:') as archive:
        members = archive.getmembers()
        if (len(members) != 4 or {m.name for m in members} != set(INPUTS) | {'sha256sums'}
                or any(not m.isfile() or m.size > 16384 for m in members)):
            raise ValueError('Unexpected private input archive')
        for name, (size, checksum) in INPUTS.items():
            data = archive.extractfile(name).read()
            if len(data) != size or digest(data) != checksum:
                raise ValueError('Wrong firmware/calibration: ' + name)
            files[name] = data
    if TIMING_SLEEP is not None:
        files['sleep'] = TIMING_SLEEP
    files['identity.json'] = (json.dumps(identity) + '\n').encode()
    files['sha256sums'] = ''.join(f'{digest(data)}  {name}\n' for name, data in files.items()).encode()
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w') as archive:
        for name, data in files.items():
            member = tarfile.TarInfo(name)
            member.size, member.mode = len(data), 0o700 if name == 'sleep' else 0o600
            archive.addfile(member, io.BytesIO(data))
    return out.getvalue()


def runtime_manifest(text):
    entries = {}
    for line in text.splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  (/[-A-Za-z0-9_./]+)', line)
        if not match or '..' in Path(match[2]).parts or match[2] in entries:
            raise ValueError('Invalid runtime manifest')
        entries[match[2]] = match[1]
    fixed = {'/usr/sbin/q1000k-pon-validate', '/usr/sbin/q1000k-pon-bench',
             '/lib/q1000k-xgspon/common.sh', '/usr/share/libubox/jshn.sh',
             '/usr/sbin/q1000k-omci', '/usr/libexec/q1000k-omci-config', '/usr/share/q1000k-bench/capabilities.json'}
    modules = set(entries) - fixed
    if not fixed <= entries.keys() or len(modules) != 9 or {
            Path(p).stem.replace('-', '_') for p in modules} != set(MODULES):
        raise ValueError('Manifest must include both helpers and all nine PON modules')
    if any(not re.fullmatch(r'/lib/modules/[0-9][0-9A-Za-z.+-]*/[-a-z0-9_]+\.ko', p) for p in modules):
        raise ValueError('Unexpected runtime module path')
    return text


def artifact_pin(artifact):
    checkpoint = json.loads((artifact / 'checkpoint.json').read_text())
    selection = json.loads((artifact / 'selection.json').read_text())
    revision = selection['revision']
    if (checkpoint['status'] != 'passed' or checkpoint['revision'] != revision
            or selection['profile'] != 'activation' or not re.fullmatch('[0-9a-f]{40}', revision)):
        raise ValueError('Require a passed activation build checkpoint')
    checksum = digest((artifact / IMAGE).read_bytes())
    if checksum != checkpoint['sha256']:
        raise ValueError('Image differs from build checkpoint')
    sums = runtime_manifest((artifact / 'runtime-sha256sums').read_text())
    return dict(schema_version=1, revision=revision, image=IMAGE, image_sha256=checksum, runtime=sums)


def build_single_file(artifact, output, timing_helper=None):
    if PIN is not None:
        raise ValueError('Generate from the source collector')
    pin = artifact_pin(artifact)
    timing = timing_helper.read_bytes() if timing_helper else None
    if timing is not None:
        if len(timing) > 131072 or timing[:6] != b'\x7fELF\x02\x01' or timing[18:20] != b'\xb7\x00':
            raise ValueError('Timing helper must be a bounded little-endian AArch64 ELF')
        pin['timing_sleep_sha256'] = digest(timing)
    marker = 'PIN = ' + 'None\n'
    source = Path(__file__).read_text()
    if source.count(marker) != 1:
        raise ValueError('Ambiguous embedding marker')
    if timing is not None:
        timing_marker = 'TIMING_SLEEP = ' + 'None\n'
        if source.count(timing_marker) != 1:
            raise ValueError('Ambiguous timing embedding marker')
        source = source.replace(timing_marker, 'TIMING_SLEEP = bytes.fromhex(' + repr(timing.hex()) + ')\n')
    with output.open('x') as stream:
        stream.write(source.replace(marker, 'PIN = ' + repr(pin) + '\n'))
    output.chmod(0o700)
    print(json.dumps(dict(collector=str(output), sha256=digest(output.read_bytes()),
                         revision=pin['revision'], image_sha256=pin['image_sha256']), indent=2))


def guards(pin, idle=True):
    text = 'set -eu\nq1000k-pon-validate status\n'
    text += 'grep -qx ' + shlex.quote('Revision: ' + pin['revision']) + ' /build_info\n'
    text += "sha256sum -c <<'VALIDATION_RUNTIME_SUMS'\n" + runtime_manifest(pin['runtime']) + 'VALIDATION_RUNTIME_SUMS\n'
    if idle:
        text += 'for module in ' + ' '.join(MODULES) + '; do test ! -d /sys/module/$module; done\n'
        text += 'test ! -e /var/run/q1000k-pon-bench.lock\ntest ! -e /sys/class/net/ponraw/master\n'
        text += 'flags=$(cat /sys/class/net/ponraw/flags); test "$((flags & 1))" = 0\n'
    return text


def ssh(script, payload=None, timeout=120):
    command = SSH + (['sh', '-s'] if payload is None else [script])
    result = subprocess.run(command, input=script.encode() if payload is None else payload,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    if result.returncode:
        raise RuntimeError('Device command failed: ' + (result.stdout + result.stderr).decode(errors='replace')[-4000:])
    return result.stdout.decode(errors='replace')


def plan(skip_physical=False, rx_only=False, soak=180, physical_only=False):
    if physical_only:
        if skip_physical:
            raise ValueError('--physical-only cannot be combined with --skip-physical')
        return [dict(name='rx-reconnect', mode='rx', samples=300)]
    cases = [dict(name='rx-startup', mode='rx', samples=30),
             dict(name='rx-repeat-1', mode='rx', samples=30),
             dict(name='rx-repeat-2', mode='rx', samples=30),
             dict(name='rx-soak', mode='rx', samples=soak)]
    if not skip_physical:
        cases.append(dict(name='rx-reconnect', mode='rx', samples=300))
    if not rx_only:
        cases.append(dict(name='activation', mode='activate', samples=180))
    return cases


# Names describe sessions; IDs map to the published hypothesis/test matrix.
PHYSICAL = {'rx-reconnect', 'rx-confirm', 'activation-reconnect', 'rx-dark-start',
            'rx-short-outage', 'rx-long-outage'}

ISOLATED_TESTS = [
 'normal-gate-no-producer', 'prbs7-existing-clock', 'prbs7-native-clock',
 'prbs23', 'prbs31', 'all-zero', 'all-one', 'alternating', 'ben-inverted',
 'oem-eye0', 'oem-eye1', 'sir-eye0-tssi', 'sir-eye1-tssi', 'ben-forced-off',
 'loop-restart', 'in-timeslot-no-grants', 'generator-tx-disabled', 'prbs7-repeat',
]

MEASUREMENT_TESTS = [
 'passive-prbs7', 'mpd-tx-disabled', 'mpd-prbs7', 'mpd-ben-off',
 'mpd-ben-inverted', 'mpd-oem-eye0', 'mpd-sir-eye0', 'mpd-loop-restart',
 'mpd-all-one', 'mpd-all-zero', 'mpd-alternating', 'mpd-existing-clock', 'mpd-repeat',
]

OUTPUT_TESTS = [
 'internal-only-passive', 'both-gates-passive', 'board-only-passive',
 'both-gates-fixed-monitor', 'internal-only-fixed-monitor', 'board-only-fixed-monitor',
 'oem-eye0-fixed-monitor', 'sir-eye0-fixed-monitor', 'no-producer-fixed-monitor',
 'ben-inverted-fixed-monitor', 'existing-clock-fixed-monitor', 'ben-off-fixed-monitor',
 'loop-restart-fixed-monitor', 'all-one-fixed-monitor', 'all-zero-fixed-monitor',
 'both-gates-repeat', 'both-disabled-fixed-monitor',
]
OUTPUT_REGISTERS = [0x488,0x3a4,0xf0,0xfe,0x3a4,0x488,0x33c,0x66,0x64,0x6a,
                    0x3c4,0x3c8,0x3e0,0x83,0xfb,0x100,0x108,0x120,0x124,0x128,
                    0x130,0x13c,0x208,0x210,0x214,0x248,0x160,0x3e4,0xb4,0xb8]


# NAND en7572.ko .rodata+0xf8: 156 {key,current_uA} pairs, keys 100..255.
# Zero ADC -> key256 -> unavailable, never zero current. Not connector power.
OEM_MPD_CURRENT = (
    63,64,65,67,68,70,71,73,75,76,78,80,81,83,85,87,
    89,91,93,95,97,99,101,103,105,108,110,112,115,117,120,123,
    125,128,131,134,137,140,143,146,149,152,156,159,163,166,170,173,
    177,181,185,189,193,198,202,206,211,215,220,225,230,235,240,245,
    251,256,262,268,273,279,286,292,298,305,311,318,325,332,340,347,
    355,362,370,378,387,395,404,413,422,431,440,450,460,470,480,491,
    501,512,524,535,547,559,571,584,596,609,623,636,650,665,679,694,
    709,725,741,757,773,790,808,825,843,862,881,900,920,940,960,981,
    1003,1025,1047,1070,1094,1118,1142,1167,1193,1219,1245,1273,1301,1329,1358,1388,
    1418,1449,1481,1514,1547,1581,1615,1651,1687,1724,1761,1800,
)

def output_result(records, test_id, owner):
    """Validate the actual gates/timing before interpreting internal sensors."""
    result = dict(records=records, registers=OUTPUT_REGISTERS,
                  conversion_ready_verified=False, connector_emission_verified=False)
    gates = 1 if test_id in (32,36) else 2 if test_id in (34,37) else 0 if test_id == 48 else 3
    fixed = test_id >= 35
    if (len(records) != 3 or [r.get('phase') for r in records] != [0,1,2]
            or any(r.get('output_version') != 1 or r.get('id') != test_id or r.get('gates') != gates
                   or r.get('fixed_monitor') is not fixed for r in records)):
        return result, 'containment-failure'
    start, stop = owner.get('window_ns',0), owner.get('disabled_ns',0)
    if not 0 < start < stop or stop-start > 6_000_000_000:
        return result, 'containment-failure'
    if bool(owner.get('enabled_ns',0)) != bool(gates & 1):
        return result, 'containment-failure'
    decoded, all_values = [], []
    previous_end = 0
    for phase, record in enumerate(records):
        rows = record.get('samples', [])
        if len(rows) != 5: return result, 'containment-failure'
        phase_rows = []
        for row in rows:
            v, valid = row.get('v', []), row.get('valid',0)
            begin, end = row.get('begin_ns',0), row.get('end_ns',0)
            if (len(v) != 30 or any(type(x) is not int or not 0 <= x <= 0xffffffff for x in v)
                    or row.get('error') != 0 or not previous_end <= begin < end
                    or (phase == 0 and end > start) or (phase == 1 and not start <= begin < end <= stop)
                    or (phase == 2 and begin < stop)):
                return result, 'containment-failure'
            previous_end = end
            target = gates if phase == 1 else 0
            if (not valid & (1 << 12) or (not bool(v[12] & 512)) != bool(target & 1)
                    or row.get('board_disabled') != int(not target & 2)):
                return result, 'containment-failure'
            hardware = [((v[i] >> 7) & 0xffff) if valid & (1 << i) else None for i in (1,4)]
            mailbox = v[2] if valid & 4 else None
            selected = fixed and all(valid & (1<<i) for i in (17,20,22)) and (v[17] & (1 << 26)) and v[20] & 0x3f00 == 0x2400 and v[22] & 0x70 == 0x40
            key = ((256 - (v[6] >> 7)) & 0xffff) if valid & (1 << 6) else None
            current = OEM_MPD_CURRENT[key-100] if selected and key is not None and 100 <= key <= 255 else None
            phase_rows.append(dict(begin_ns=begin,end_ns=end,hardware_tssi=hardware,mailbox_tssi=mailbox,
                mailbox_matches_bracket=mailbox in hardware if mailbox is not None and None not in hardware else None,
                ben=[(v[i]&1) if valid & (1<<i) else None for i in (0,5)],
                reporting_status=v[3] if valid & 8 else None,raw_monitor=v[6] if valid & 64 else None,
                monitor_key=key,monitor_current_uA_oem=current,
                monitor_current_valid=current is not None,monitor_selection_verified=bool(selected),
                tx_power_nW=v[7]*100 if valid & 128 else None,
                mcu_idle=v[13],reporting_flags=v[14],ocp_control=v[26],ocp_status=v[27]))
            all_values.append((v, valid))
        decoded.append(phase_rows)
    result['decoded'] = decoded
    # These are the settings the experiment promises to leave unchanged after
    # setup. Autonomous hardware/MCU changes remain visible and invalidate the
    # fixed-settings comparison; they are evidence, not cleanup failures.
    masks = {15:0xc,17:1<<26,18:0xff00,19:0xfffff00,20:0x3f01,21:0x70060,
             22:0x71,23:0xfff1fff,24:0x1fff0000,25:0xfff0000,28:0xffffffff,29:0xffffffff}
    result['settings_changed'] = [hex(OUTPUT_REGISTERS[i]) for i,mask in masks.items()
        if len({v[i]&mask for v,valid in all_values if valid & (1<<i)}) > 1]
    result['settings_observation_complete'] = all(valid & (1<<i) for _,valid in all_values for i in masks)
    result['fixed_settings_verified'] = result['settings_observation_complete'] and not result['settings_changed']
    result['by_phase'] = []
    for phase in decoded:
        def bounds(values):
            values=[x for x in values if x is not None]
            return [min(values),max(values)] if values else None
        result['by_phase'].append(dict(
            hardware_tssi=bounds([v for r in phase for v in r['hardware_tssi']]),
            mailbox_tssi=bounds([r['mailbox_tssi'] for r in phase]),
            ben=sorted({v for r in phase for v in r['ben'] if v is not None}),
            reporting_status=sorted({r['reporting_status'] for r in phase if r['reporting_status'] is not None}),
            monitor_current_uA_oem=bounds([r['monitor_current_uA_oem'] for r in phase]),
            tx_power_nW=bounds([r['tx_power_nW'] for r in phase])))
    result['on_ben_observed'] = 1 in result['by_phase'][1]['ben']
    result['sensor_observation_complete'] = all(valid & 0xff == 0xff for _,valid in all_values)
    if not result['sensor_observation_complete']:
        return result, 'measurement-unavailable'
    return result, 'gate-assertion-observed' if result['on_ben_observed'] else 'no-gate-assertion-observed'

def monitor_result(rows, test_id):
    records = [r for r in rows if isinstance(r, dict) and r.get('mpd_version') == 1]
    if len(records) != 1 or records[0].get('id') != test_id:
        return {}, 'containment-failure'
    record = records[0]
    probes = record.get('probes', [])
    if len(probes) != 3 or [p.get('phase') for p in probes] != [0, 1, 2]:
        return record, 'containment-failure'
    for p in probes:
        if (p.get('error') != 0 or p.get('restore_error') != 0 or p.get('restored') is not True
                or p.get('active') is not (test_id != 19)
                or p.get('end_ns', 0) <= p.get('begin_ns', 0)
                or p.get('sample_error') != [0, 0, 0]
                or len(p.get('values', [])) != 3 or any(len(v) != 9 for v in p['values'])
                or len(p.get('valid', [])) != 3):
            return record, 'containment-failure'
        if test_id != 19 and p.get('selected_valid') != 7:
            return record, 'containment-failure'
    selected_row = 0 if test_id == 19 else 1
    record['tx_enabled_by_phase'] = [not bool(p['values'][selected_row][7] & 512)
        if p['valid'][selected_row] & 128 else None for p in probes]
    if record['tx_enabled_by_phase'] != [False, test_id != 20, False]:
        return record, 'containment-failure'
    record['raw_monitor_by_phase'] = [p['values'][selected_row][0]
        if p['valid'][selected_row] & 1 else None for p in probes]
    record['raw_tssi_by_phase'] = [p['values'][selected_row][1]
        if p['valid'][selected_row] & 2 else None for p in probes]
    # A read response is not a conversion-completion indication.
    record['conversion_ready_verified'] = False
    record['monitor_changed_between_tx_phases'] = (len(set(record['raw_monitor_by_phase'])) > 1
        if all(v is not None for v in record['raw_monitor_by_phase']) else None)
    return record, ('measured-response' if record['monitor_changed_between_tx_phases'] else
                    'measured-flat' if record['monitor_changed_between_tx_phases'] is False else 'measurement-unavailable')


def discovery_plan(args):
    if args.physical_only and args.skip_physical:
        raise ValueError('--physical-only cannot be combined with --skip-physical')
    if getattr(args, 'suite', 'legacy') in ('isolated', 'measurement', 'output'):
        if args.physical_only or args.identity:
            raise ValueError('Isolated suite requires disconnected fiber and no subscriber identity')
        if args.rx_only:
            raise ValueError('Isolated suite emits bounded test patterns; use --suite legacy --rx-only for RX-only work')
        cases = [dict(name=f'isolated-{i}', label=label, mode='isolated', samples=30,
                      ids=[f'I{i:02d}'])
                 for i,label in enumerate(OUTPUT_TESTS if args.suite == 'output' else MEASUREMENT_TESTS if args.suite == 'measurement' else ISOLATED_TESTS,
                                          32 if args.suite == 'output' else 19 if args.suite == 'measurement' else 1)]
        if getattr(args, 'cases', None):
            selected = set(args.cases.split(','))
            if not selected <= {c['name'] for c in cases}: raise ValueError('Unknown isolated case')
            cases = [c for c in cases if c['name'] in selected]
        return cases
    rx_only = args.rx_only or args.physical_only or not args.identity
    if getattr(args, 'suite', 'legacy') == 'topology':
        if args.physical_only:
            raise ValueError('Topology comparisons keep the fiber connected')
        cases = [dict(name='rx-startup', mode='rx', samples=30, ids=['T01','T02','T03'])]
        if not rx_only:
            for name, factory, ranging, samples in [('strict',False,1,300),
                    ('oem',True,1,300), ('eqd',True,3,300), ('repeat',True,1,600)]:
                cases.append(dict(name='activation-omci-topology-'+name, mode='activate',
                    samples=samples, ranging_mode=ranging, dot1x_oem=factory, live_add=3,
                    key_inline=True, initial_key_readback=True, omci_min_len=60, alloc_revoke=False,
                    ids=['O1','O2','O3','O4','O5','O6','O7','O8','Q1','Q2','Q3','Q4','Q5']))
        if getattr(args, 'cases', None):
            selected = set(args.cases.split(','))
            if not selected <= {c['name'] for c in cases}: raise ValueError('Unknown topology case')
            cases = [c for c in cases if c['name'] in selected]
        return cases
    if getattr(args, 'suite', 'legacy') in ('omci', 'continuity', 'topology'):
        if args.physical_only:
            raise ValueError('OMCI comparisons use connected fiber without physical cycling')
        cases = [dict(name='rx-startup', mode='rx', samples=30, ids=['T01','T02','T03'])]
        if not rx_only:
            for name, mode, minimum, revoke, samples in [
                    ('fixed',1,60,False,300), ('oem',3,60,False,300),
                    ('min48',1,48,False,300), ('revoke',1,60,True,300),
                    ('repeat',1,60,False,600),
                    ('live-gem',1,60,False,240), ('live-tcont',1,60,False,240),
                    ('live-both',1,60,False,300), ('live-oem',3,60,False,300),
                    ('live-repeat',1,60,False,600)]:
                if args.suite == 'continuity' and name != 'fixed' and not name.startswith('live-'):
                    continue
                cases.append(dict(name='activation-omci-'+name, mode='activate', samples=samples,
                    ranging_mode=mode, key_inline=True, initial_key_readback=True,
                    omci_min_len=minimum, alloc_revoke=revoke,
                    live_add={'live-gem':1,'live-tcont':2,'live-both':3,'live-oem':3,'live-repeat':3}.get(name,0),
                    ids=['O1','O2','O3','O4','O5','O6','O7','O8']))
        if getattr(args, 'cases', None):
            selected = set(args.cases.split(','))
            if not selected <= {c['name'] for c in cases}: raise ValueError('Unknown OMCI case')
            cases = [c for c in cases if c['name'] in selected]
        return cases
    if getattr(args, 'suite', 'legacy') == 'registration':
        if args.physical_only:
            raise ValueError('Registration comparisons use connected fiber without physical cycling')
        cases = [dict(name='rx-startup', mode='rx', samples=30, ids=['T01','T02','T03'])]
        if not rx_only:
            for name, mode, keys in [('reference',0,0), ('range',1,0), ('keys',0,1),
                    ('combined',1,1), ('resync-retain',2,1), ('oem-direct',3,1), ('repeat',1,1)]:
                cases.append(dict(name='activation-reg-'+name, mode='activate', samples=300,
                    ranging_mode=mode, key_inline=bool(keys),
                    ids=['H1','H2','H3','H4','H5','D01','D02','D03','D04','D05','D06']))
        if getattr(args, 'cases', None):
            selected = set(args.cases.split(','))
            if not selected <= {c['name'] for c in cases}: raise ValueError('Unknown registration case')
            cases = [c for c in cases if c['name'] in selected]
        return cases
    if getattr(args, 'suite', 'legacy') == 'tx' and not args.physical_only:
        cases = [dict(name='rx-startup', mode='rx', samples=30, ids=['T01','T02','T03'])]
        if not rx_only:
            cases += [dict(name='activation-tx-'+name, mode='activate', samples=600,
                           ids=['T01','T02','T03','T04','T05','T06','D01','D02','D03','D04','D05','D06'])
                      for name in ('baseline','coalesced','repeat','quiet')]
        if getattr(args, 'cases', None):
            selected = set(args.cases.split(','))
            if not selected <= {c['name'] for c in cases}: raise ValueError('Unknown TX suite case; use --suite legacy for recovery cases')
            cases = [c for c in cases if c['name'] in selected]
        return cases
    cases = []
    if not args.physical_only:
        cases = [dict(name=n, mode='rx', samples=30, ids=['B02']) for n in
                 ('rx-startup', 'rx-repeat-1', 'rx-repeat-2')]
        cases.append(dict(name='rx-soak', mode='rx', samples=args.soak, ids=['B03']))
        if not rx_only:
            cases.append(dict(name='activation', mode='activate', samples=240, ids=['A01','A04','D01','D02','D03','D04','D05','D06']))
    if not args.skip_physical:
        cases += [dict(name='rx-reconnect', mode='rx', samples=600, ids=['R00','R01','R02','R03','R04','R05','R06','R07']),
                  dict(name='rx-confirm', mode='rx', samples=600, ids=['R08'], requires_winner=True)]
        if not rx_only:
            cases.append(dict(name='activation-reconnect', mode='activate', samples=240, ids=['R09']))
    if not rx_only and not args.physical_only:
        cases += [dict(name='activation-quiet', mode='activate', samples=240, ids=['A02','A04']),
                  dict(name='activation-long-sn', mode='activate', samples=240, ids=['A03'], requires_sn_reset=True)]
    optional = [dict(name=n, mode='rx', samples=600, ids=[i]) for n,i in
                [('rx-dark-start','R10'),('rx-short-outage','R11'),('rx-long-outage','R11')]]
    if getattr(args, 'cases', None):
        selected = set(args.cases.split(','))
        known = {c['name'] for c in cases + optional}
        if not selected <= known: raise ValueError('Unavailable/unknown case selection: ' + ', '.join(sorted(selected-known)))
        cases = [c for c in cases + optional if c['name'] in selected]
        for case in cases:
            if case.pop('requires_sn_reset', False):
                case['sn_comparison_selection'] = 'explicit'
    for case in cases:
        case['recovery_actions'] = getattr(args, 'recovery_action', None) or '1,2,3,4,5,6,7'
        if case.get('requires_winner') and getattr(args, 'recovery_action', None):
            case.pop('requires_winner')
    return cases


def trace_summary(records):
    retained = [r for r in records if isinstance(r,dict) and r.get('trace_version') == 1]
    events = [r for r in retained if not r.get('first')]
    critical = [r for r in records if isinstance(r,dict) and r.get('critical_version') == 1]
    retained += critical
    positions = {}
    for r in critical: positions.setdefault(r.get('stack_generation',1), set()).add(r['position'])
    expected = {}
    for r in records:
        if isinstance(r,dict) and r.get('critical_header') == 1:
            stack = r.get('stack_generation',1)
            expected[stack] = max(expected.get(stack,0), r['newest'])
    critical_gaps = sum(max(expected.get(k,0), max(positions.get(k, {0}))) - len(positions.get(k, set()))
                        for k in set(expected) | set(positions))
    counts = {}; per_stack = {}
    for r in records:
        if isinstance(r,dict) and r.get('trace_count') == 1:
            per_stack.setdefault(r.get('stack_generation',1), {})[f"{r['event']}:{r['id']}"] = {k:r[k] for k in ('count','errors')}
    for entries in per_stack.values():
        for key, value in entries.items():
            total = counts.setdefault(key, dict(count=0,errors=0))
            for field in total: total[field] += value[field]
    groups = {}
    for r in events: groups.setdefault(r.get('stack_generation',1), set()).add(r['seq'])
    gaps = 0
    for values in groups.values():
        seen = sorted(values)
        gaps += sum(b-a-1 for a,b in zip(seen, seen[1:]))
        if seen: gaps += seen[0]-1
    def counter(event, ident):
        return counts.get(f'{event}:{ident}', {}).get('count',0)
    milestones = dict(profile_verified=counter(10,1)-counts.get('10:1',{}).get('errors',0),
        profiles_coalesced=sum(counter(24,i) for i in range(4)),
        profile_commit_observed=any(r['event']==14 and r['id']==1 and r['b'] and r['c'] for r in retained),
        sn_request_interrupts=counter(8,2),sn_sent_interrupts=counter(8,3),
        ranging_request_interrupts=counter(8,4),registration_sent_interrupts=counter(8,5),
        local_assignment_observed=any(r['event']==15 and r['a']==1 for r in retained),
        ranging_accepted=counter(11,4)-counts.get('11:4',{}).get('errors',0))
    return dict(critical_records=sum(map(len,positions.values())), critical_sequence_gaps=critical_gaps,
                critical_concurrent_wrap=any(isinstance(r,dict) and 'critical_gap' in r for r in records),
                milestones=milestones, events=sum(map(len,groups.values())), counters=counts, internal_sequence_gaps=gaps,
                concurrent_wrap=any(isinstance(r,dict) and 'trace_gap' in r for r in records),
                sn_threshold_reset=bool(counter(16,1)) or any(r['event']==16 and r['id']==1 for r in retained),
                first_reset=min((r for r in retained if r['event']==16), key=lambda r:r['ns'], default=None),
                first_fault=min((r for r in retained if r['event']==2), key=lambda r:r['ns'], default=None),
                generation_changes=sorted({r['generation'] for r in retained}))


def test_outcomes(case, result, text):
    """Keep selectable capabilities separate from branches actually observed."""
    stages = result['stages']; outcome = {}
    for ident in case.get('ids', []):
        outcome[ident] = 'not-run'
        if ident.startswith('Q'):
            o=result.get('omci_experiment',{}); t=o.get('topology',{}); p=o.get('provisioning',{})
            responses=p.get('response_results',[])
            queue_sets=[r for r in responses if r['class_id']==277 and r['opcode']==8]
            gem_creates=[r for r in responses if r['class_id']==268 and r['opcode']==4 and r['result']==0]
            dot1x=[r for r in responses if r['class_id']==290 and r['opcode']==8]
            outcome[ident]={
                'Q1':'queue-topology-submitted' if t.get('fully_described_queues') else 'topology-not-observed',
                'Q2':'queue-sets-accepted' if queue_sets and all(r['result']==0 for r in queue_sets) else 'queue-acceptance-not-established',
                'Q3':'dot1x-controls-accepted' if dot1x and all(r['result']==0 for r in dot1x) else 'dot1x-acceptance-not-established',
                'Q4':'upstream-gem-create-observed' if any(r['entity_id'] in range(1023,1027) for r in gem_creates) else 'upstream-gems-not-established',
                'Q5':'service-and-traffic-observed' if result.get('provisioned') and any(result.get('traffic',{}).values()) else 'service-not-verified',
            }[ident]
            if not o.get('critical_evidence_complete'): outcome[ident] += '; critical-evidence-incomplete'
            continue
        if ident.startswith('O'):
            o = result.get('omci_experiment', {})
            outcome[ident] = {
                'O1': 'initial-enable-readback-observed' if o.get('initial_key_readback_accepted') else 'initial-readback-not-reached',
                'O2': 'runt-omci-authenticated' if o.get('ethernet_runt_omci_delivered') and o.get('authenticated_rx') else 'no-authenticated-runt-observed',
                'O3': 'allocation-session-preserved' if o.get('allocations_kept_session') and o.get('epoch_reconciles') else 'allocation-preservation-not-observed',
                'O4': 'deferred-response-consumed' if o.get('deferred_native_consumed') else 'deferred-response-not-observed',
                'O5': 'tx-auth-rejection-observed' if o.get('tx_auth_rejected') else 'no-tx-auth-rejection-observed',
                'O7': 'native-descriptor-outcomes-observed' if o.get('native_tx',{}).get('events') else 'native-descriptor-path-not-observed',
                'O8': 'live-addition-observed' if o.get('live_additions',{}).get('completed') else 'live-addition-not-observed',
                'O6': 'provisioning-and-traffic-observed' if result.get('provisioned') and any(result.get('traffic', {}).values()) else 'service-not-verified',
            }[ident]
            if not o.get('critical_evidence_complete'): outcome[ident] += '; critical-evidence-incomplete'
            continue
        if ident.startswith('H'):
            a = result.get('registration', {})
            outcome[ident] = {
                'H1': 'timing-observed' if a.get('ranging_to_ack_ms') else 'no-paired-acknowledgement',
                'H2': 'key-report-enqueued' if a.get('key_reports_enqueued') else 'no-key-report-enqueued',
                'H3': 'boundary-registers-observed' if a.get('complete_snapshots') else 'no-complete-boundary-snapshot',
                'H4': 'eqd-sequence-observed' if a.get('narrow_ranging_completed') else 'reference-or-narrow-path-not-reached',
                'H5': 'tx-input-audit-observed' if a.get('tx_audit_passed') else 'tx-input-audit-not-reached',
            }[ident]
            if not a.get('critical_evidence_complete'): outcome[ident] += '; critical-evidence-incomplete'
            continue
        if ident in ('B02','B03'): outcome[ident] = result['status']
        elif ident in ('A01','A02','A03','D01') and 'activation' in stages:
            outcome[ident] = stages['activation']
        elif ident.startswith('T'):
            if ident == 'T01': outcome[ident] = 'internal-observations' if result.get('tx',{}).get('samples') else 'not-run'
            elif ident in ('T02','T03'): outcome[ident] = 'register-observations' if result.get('tx',{}).get('mac_samples') else 'not-run'
            elif ident == 'T04': outcome[ident] = 'coalescing-observed' if result.get('trace',{}).get('milestones',{}).get('profiles_coalesced') else 'baseline-or-no-eligible-profile'
            elif ident == 'T05': outcome[ident] = 'critical-events-retained' if result.get('trace',{}).get('critical_records') else 'not-run'
            elif ident == 'T06': outcome[ident] = 'observed; compare matched sessions'
        elif ident == 'A04':
            outcome[ident] = 'profile-observed' if result.get('trace',{}).get('milestones',{}).get('profile_verified') else 'no-verified-profile-observed'
        elif ident == 'D02' and 'provisioning' in stages: outcome[ident] = stages['provisioning']
        elif ident == 'D03' and any(v.get('samples', 0) for v in result.get('security', {}).values()):
            outcome[ident] = 'key-and-counter-observations; encrypted-traffic-unproven'
        elif ident == 'D04' and 'wan' in stages: outcome[ident] = stages['wan']
        elif ident == 'D05' and result.get('traffic'): outcome[ident] = 'observed; see-per-probe-results'
        elif ident == 'D06' and 'traffic-soak' in stages: outcome[ident] = stages['traffic-soak']
        elif ident in ('R00','R09','R10','R11') and 'passive' in stages: outcome[ident] = stages['passive']
        elif ident == 'R08':
            outcome[ident] = 'independent-recovery-observed' if result.get('independent_recovery') else stages.get('recovery','not-run')
    for action, stable in re.findall(r'^recovery_action=([1-7]) phase=observed stable=(\d+)$',text,re.M):
        ident = 'R0'+action
        if ident in outcome:
            outcome[ident] = 'frames-restored-after-sequence' if int(stable)>=5 else 'no-reacquisition'
    return outcome


def case_outcome(result, trace_required=True, critical_only=False):
    # A functional negative is useful evidence; containment cannot be waived.
    if result['stages'].get('cleanup') != 'passed' or result['stages'].get('failure') == 'containment':
        return 'containment-failure'
    if result.get('timing_errors'):
        return 'inconclusive'
    physical = result.get('physical_control')
    if not result['diagnostics_pairs_valid'] or (physical and (not physical['confirmed'] or physical.get('dark_samples',0) < result.get('required_dark_samples',15))):
        return 'inconclusive'
    t = result.get('trace', {})
    if critical_only and not result.get('registration', {}).get('critical_evidence_complete'):
        return 'inconclusive'
    if trace_required and not critical_only and (not t.get('events') or t.get('internal_sequence_gaps') or t.get('concurrent_wrap')):
        return 'inconclusive'
    if result['returncode'] == 2 and result['stages'].get('outcome') == 'functional-negative':
        return 'functional-negative'
    if result['returncode'] == 3: return 'inconclusive'
    return 'observed' if result['returncode'] == 0 else 'inconclusive'


def json_records(text):
    decoder, values, offset = json.JSONDecoder(), [], 0
    while offset < len(text):
        if text[offset] not in '{[':
            offset += 1
            continue
        try:
            # Decode in place: slicing the remaining capture for every
            # record makes large event histories take quadratic time.
            value, offset = decoder.raw_decode(text, offset)
            values.append(value)
        except ValueError:
            offset += 1
    return values


def burst_segments(mac):
    segments = []
    for row in mac:
        value = row.get('registers',{}).get('5944')
        generation = row.get('hardware_generation')
        if type(value) is not int or type(generation) is not int: continue
        stack = row.get('stack_generation',1)
        if not segments or (segments[-1]['stack_generation'],segments[-1]['hardware_generation']) != (stack,generation):
            segments.append(dict(stack_generation=stack, hardware_generation=generation, first=value, last=value,
                samples=1, delta_modulo_32=0, decreases=0, begin_ns=row.get('begin_ns'), end_ns=row.get('end_ns')))
        else:
            s = segments[-1]
            s['delta_modulo_32'] += (value - s['last']) & 0xffffffff
            s['decreases'] += value < s['last']
            s.update(last=value, samples=s['samples']+1, end_ns=row.get('end_ns'))
    return segments


def registration_summary(records):
    """Separate local enqueue/readback evidence from OLT acceptance.

    Deduplicate the ordinary/critical/first copies. Pair only within one
    stack load and between resets. All timestamps are host-side evidence.
    """
    unique = {}
    for r in records:
        if isinstance(r, dict) and (r.get('trace_version') == 1 or r.get('critical_version') == 1):
            unique[(r.get('stack_generation', 1), r['seq'])] = r
    events = [unique[k] for k in sorted(unique)]
    starts, key_starts, rcu_starts = {}, {}, {}
    ranges, keys, rcu, snapshots = [], [], [], []
    current = None
    for e in events:
        stack, ev, ident = e.get('stack_generation', 1), e['event'], e['id']
        if ev == 16:
            starts = {k:v for k,v in starts.items() if k[0] != stack}
            key_starts = {k:v for k,v in key_starts.items() if k[0] != stack}
        if ev == 27:
            if ident == 1: starts[(stack, e['c'])] = e['ns']
            if ident == 10: key_starts[(stack, e['a'])] = e['ns']
            if ident == 4: rcu_starts[stack] = e['ns']
            if ident == 5 and stack in rcu_starts:
                rcu.append((e['ns'] - rcu_starts.pop(stack)) / 1e6)
            if ident == 15 and e['result'] == 0:
                pending, values = (starts, ranges) if e['a'] == 9 else (key_starts, keys)
                if e['a'] in (5, 9) and (stack, e['b']) in pending:
                    values.append((e['ns'] - pending.pop((stack, e['b']))) / 1e6)
        if ev == 26:
            if ident == 255:
                current = dict(stack_generation=stack, stage=e['a'], sequence=e['b'],
                    hardware_generation=e['c'], expected_registers=e['d'], ns=e['ns'], registers={})
                snapshots.append(current)
            elif current is not None and current['stack_generation'] == stack and e['result'] == 0:
                if e['c'] == current['stage'] and e['d'] == current['sequence']:
                    current['registers'][f"{e['a']:04x}"] = e['b']
    trace = trace_summary(records)
    def matched(event, ident, predicate=lambda e: True):
        return sum(e['event'] == event and e['id'] == ident and predicate(e) for e in events)
    complete = [s for s in snapshots if len(s['registers']) == s['expected_registers']]
    return dict(ranging_to_ack_ms=ranges, key_request_to_enqueue_ms=keys, rcu_wait_ms=rcu,
        key_reports_enqueued=matched(27, 15, lambda e: e['a'] == 5 and e['result'] == 0),
        acknowledgements_enqueued=matched(27, 15, lambda e: e['a'] == 9 and e['result'] == 0),
        key_requests_accepted=matched(27, 10), key_confirmations_accepted=matched(27, 10, lambda e:e['c'] == 1),
        key_requests_cancelled=matched(27, 13), key_inline_fallbacks=matched(27, 12),
        tx_audit_passed=matched(27, 14, lambda e:e['result'] == 0),
        tx_audit_failed=matched(27, 14, lambda e:e['result'] != 0),
        narrow_ranging_completed=matched(27, 7, lambda e:e['result'] == 0),
        narrow_modes=sorted({e['c'] for e in events if e['event'] == 27 and e['id'] == 7 and not e['result']}),
        olt_deactivations=matched(11, 5, lambda e:e['result'] == 0),
        complete_snapshots=len(complete), snapshots=snapshots,
        critical_evidence_complete=bool(trace['critical_records'] and not trace['critical_sequence_gaps'] and not trace['critical_concurrent_wrap']),
        optical_tx_reception_proven=False, upstream_hardware_mic_verified=False,
        note='Enqueue, hardware counts and register comparisons are distinct from OLT acceptance; sparse/averaged optical readings cannot certify individual bursts.')


def omci_provisioning_summary(events):
    """Decode only typed public metadata, preserving independent failure stages.

    Caller supplies deduplicated events. Legacy event 22 has no entity/mask;
    bit 6 distinguishes new metadata from an actual entity zero/mask zero.
    GEM and QoS records remain separate when one may have been lost.
    """
    responses, operations, gems, qos, counts = [], [], [], [], {}
    for e in events:
        base = dict(seq=e['seq'], stack_generation=e.get('stack_generation', 1))
        if e['event'] == 22:
            metadata = bool(e['b'] & 64)
            row = dict(base, opcode=e['id'], class_id=e['a'],
                entity_id=e['d'] >> 16 if metadata else None,
                attribute_mask=e['d'] & 0xffff if metadata else None,
                transaction_id=e['b'] >> 16 if metadata else None,
                result=e['c'] if e['b'] & 16 else None,
                transport_error=e['result'], duplicate=bool(e['b'] & 1))
            if row['result'] or row['transport_error']:
                responses.append(row)
            if row['result'] is not None:
                key = tuple(row[k] for k in ('class_id','entity_id','opcode','attribute_mask','result'))
                if key not in counts:
                    counts[key] = {k: row[k] for k in ('class_id','entity_id','opcode','attribute_mask','result')}
                    counts[key].update(count=0, duplicates=0)
                counts[key]['count'] += 1
                counts[key]['duplicates'] += row['duplicate']
        elif e['event'] == 28 and e['id'] == 1:
            gems.append(dict(base, entity_id=e['a'] >> 16, port_id=e['a'] & 0xffff,
                tcont=e['b'] >> 16, direction=(e['b'] >> 8) & 255, key_ring=e['b'] & 255,
                alloc_id=e['c'] & 0xffff, traffic_management=(e['c'] >> 16) & 255,
                valid=bool(e['d'] & 1), allocation_sampled=bool(e['d'] & 2), error=e['result']))
        elif e['event'] == 28 and e['id'] == 2:
            qos.append(dict(base, entity_id=e['a'] >> 16, port_id=e['a'] & 0xffff,
                upstream_queue=e['b'] >> 16, upstream_descriptor=e['b'] & 0xffff,
                downstream_queue=e['c'] >> 16, downstream_descriptor=e['c'] & 0xffff,
                valid=bool(e['d']), error=e['result']))
        elif e['event'] == 29:
            operations.append(dict(base, class_id=e['a'] >> 16, entity_id=e['a'] & 0xffff,
                transaction_id=e['b'] >> 16, attribute_mask=e['b'] & 0xffff, opcode=e['c'],
                stage={0:'none',1:'validation',2:'hardware',3:'reconciliation',4:'MIB storage'}.get(e['id'], 'unknown'),
                error=e['result'], dot1x_enable=e['d'] & 255 if e['d'] & 256 else None,
                dot1x_action=(e['d'] >> 9) & 255 if e['d'] & (1 << 17) else None,
                dot1x_oem=bool(e['d'] & (1 << 18))))
    return dict(response_results=list(counts.values()), response_errors=responses,
        operations=operations, gem_configurations=gems, gem_qos=qos,
        note='Retained records only. Check critical evidence completeness; a successful ME operation does not prove a working data service.')


def topology_summary(events):
    """Reassemble only the public, explicitly whitelisted wire records.

    A missing word record leaves an incomplete row, never a zero-filled success.
    Header sequence and transaction identity distinguish retries and fragments.
    """
    rows, counts, pending = [], [], {}
    layouts = {
        262: [('alloc_id',2),('deprecated',1),('policy',1)],
        277: [('configuration',1),('maximum_size',2),('allocated_size',2),
              ('discard_reset',2),('discard_threshold',2),('related_port',4),
              ('scheduler_pointer',2),('weight',1),('backpressure_operation',2),
              ('backpressure_time',4),('backpressure_occur',2),('backpressure_clear',2)],
        278: [('tcont',2),('parent',2),('policy',1),('priority',1)],
    }
    for e in events:
        if e['event'] != 32: continue
        stack=e.get('stack_generation',1)
        if e['id'] == 1:
            counts.append(dict(stack_generation=stack, transaction_id=e['a'],
                               commands=e['b'], transport_error=e['result']))
        elif e['id'] in (2,6):
            row=dict(stack_generation=stack, seq=e['seq'], kind='upload' if e['id']==2 else 'set',
                     class_id=e['a'] >> 16, entity_id=e['a'] & 65535,
                     attribute_mask=e['b'] & 65535, sequence=e['b'] >> 16,
                     transaction_id=e['c'], length=e['d'], transport_error=e['result'],
                     complete=False, chunks={})
            if row['class_id'] not in layouts or not 0 <= row['length'] <= 26: continue
            rows.append(row); pending[(stack,row['transaction_id'],row['class_id'])]=row
        elif e['id'] in (3,4,5):
            row=pending.get((stack,e['a'] >> 16,e['a'] & 65535))
            if row is not None:
                row['chunks'][e['id']]=b''.join(e[k].to_bytes(4,'big') for k in ('b','c','d'))
    for row in rows:
        chunks=row.pop('chunks')
        if set(chunks) != {3,4,5}: continue
        data=b''.join(chunks[i] for i in (3,4,5))[:row['length']]
        row['data_hex']=data.hex(); row['values']={}; pos=0
        for i,(name,size) in enumerate(layouts[row['class_id']]):
            if row['attribute_mask'] & (1 << (15-i)):
                if pos+size > len(data): break
                row['values'][name]=int.from_bytes(data[pos:pos+size],'big'); pos+=size
        else:
            supported=(0xffff << (16-len(layouts[row['class_id']]))) & 0xffff
            row['complete']=not bool(row['attribute_mask'] & ~supported)
    uploads=[r for r in rows if r['kind']=='upload']
    queues={r['entity_id'] for r in uploads if r['class_id']==277 and r['complete'] and not r['transport_error'] and
            {'related_port','scheduler_pointer','weight'} <= r.get('values',{}).keys()}
    requests=[r for r in rows if r['kind']=='set']
    return dict(upload_commands=counts, wire_records=rows, fully_described_queues=sorted(queues),
                unresolved_queue_requests=[r for r in requests if r['entity_id'] not in queues],
                note='Wire bytes prove what was submitted locally; subsequent OLT requests test whether it used that description.')


def native_tx_summary(events):
    names = {1:'admission-drop', 2:'submitted', 3:'dma-complete', 4:'hardware-drop',
             5:'teardown-abort', 6:'dma-error', 7:'admission-retry'}
    rows, counts, balance = [], {}, {}
    for e in events:
        if e['event'] != 30:
            continue
        stage=names.get(e['id'], 'unknown')
        counts[stage]=counts.get(stage,0)+1
        row=dict(stack_generation=e.get('stack_generation',1), generation=e['generation'],
                 seq=e['seq'], ns=e['ns'], stage=stage, error=e['result'],
                 tci=e['a']>>16, opcode=(e['a']>>8)&31, message_type=(e['a']>>8)&255,
                 device_id=e['a']&255, me_class=e['b']>>16, entity_id=e['b']&65535,
                 gem=e['c']>>16, length=e['c']&65535, native_epoch_low32=e['d'])
        rows.append(row)
        key=(row['stack_generation'],row['generation'],e['a'],e['b'],e['c'],e['d'])
        b=balance.setdefault(key,dict(header=row,submitted=0,terminal=0))
        if e['id']==2: b['submitted']+=1
        elif e['id'] in (3,4,5): b['terminal']+=1
    pending=[dict(b['header'],submitted=b['submitted'],terminal=b['terminal'])
             for b in balance.values() if b['submitted']!=b['terminal']]
    return dict(events=rows,counts=counts,unbalanced=pending,
                optical_delivery_proven=False,
                note='Header-only native events; DMA completion is not an OLT acknowledgement. Compare only with complete critical evidence.')


def append_summary(events):
    rows=[dict(stack_generation=e.get('stack_generation',1),seq=e['seq'],ns=e['ns'],
               phase='begin' if e['id']==1 else 'end',error=e['result'],kind=e['a'],
               added_channels=e['b'],added_gems=e['c'],channels=e['d'])
          for e in events if e['event']==31]
    return dict(events=rows,completed=sum(r['phase']=='end' and r['error']==0 for r in rows),
                failed=sum(r['phase']=='end' and r['error']!=0 for r in rows))


def omci_summary(records):
    """Deduplicate polling snapshots; never infer OLT receipt from local TX."""
    unique = {}
    for row in records:
        if isinstance(row, dict) and (row.get('trace_version') == 1 or row.get('critical_version') == 1):
            unique[(row.get('stack_generation', 1), row['seq'])] = row
    events = [unique[k] for k in sorted(unique)]
    control = [e for e in events if e['event'] == 27]
    def count(ident, predicate=lambda e: True):
        return sum(e['id'] == ident and predicate(e) for e in control)
    return dict(
        provisioning=omci_provisioning_summary(events), topology=topology_summary(events),
        native_tx=native_tx_summary(events), live_additions=append_summary(events),
        ethernet_runt_omci_delivered=count(30, lambda e: bool(e['a'] & (1 << 12))),
        rx_descriptor_words=sorted({f"0x{e['a']:08x}" for e in control if e['id'] == 30}),
        rx_guard_rejected=count(31, lambda e: e['result'] != 0),
        authenticated_rx=count(32, lambda e: e['result'] == 0),
        rx_auth_rejected=count(32, lambda e: e['result'] != 0),
        tx_auth_rejected=count(37, lambda e: e['result'] != 0),
        replies_queued=count(39, lambda e: e['result'] == 0),
        reply_queue_errors=count(39, lambda e: e['result'] != 0),
        allocations_kept_session=count(40, lambda e: e['result'] == 0),
        replies_deferred=count(41), native_consumed=count(42, lambda e: e['result'] == 0),
        deferred_native_consumed=count(42, lambda e: e['result'] == 0 and bool(e['d'])),
        reply_drops=count(43), reply_expired=count(43, lambda e: bool(e['d'])),
        epoch_reconciles=count(45, lambda e: e['result'] == 0),
        initial_key_readback_accepted=count(22),
        minimums=sorted({e['b'] for e in control if e['id'] == 36 and e['result'] == 0}),
        critical_evidence_complete=registration_summary(records)['critical_evidence_complete'],
        upstream_delivery_proven=False,
        note='Retained event counts. Native consumption is not OLT acknowledgement; gaps make totals incomplete. Full service still requires provisioning and traffic.')


def summarize(text, returncode=0, events=None):
    records = json_records(text)
    live_records = json_records(re.sub(r'postmortem_begin.*?postmortem_end', '', text, flags=re.S))
    rx = [r for r in live_records if isinstance(r, dict) and 'receiver_version' in r]
    diag = [r for r in live_records if isinstance(r, dict) and 'diagnostics_version' in r]
    stages = dict(re.findall(r'^validation_stage name=(\S+) status=(\S+)$', text, re.M))
    pairs_valid = bool(rx) and len(rx) == len(diag)
    if pairs_valid:
        pairs_valid = all(0 <= d.get('sampled_ms', -1) - r.get('sampled_ms', -2) <= 1500
                          for r, d in zip(rx, diag))
    power = [10 * math.log10(r['rx_power_nw']/1000000) for r in rx
             if r.get('rx_power_valid') is True and r.get('rx_power_nw', 0) > 0]
    omci = [r for r in records if isinstance(r, dict) and 'agent_operational' in r]
    traffic = {f'ipv{family}_{kind}': int(code) == 0 for family, kind, code in re.findall(
        r'^traffic_result family=([46]) kind=(\S+) rc=(\d+)$', text, re.M)}
    network = [r for r in records if isinstance(r, dict) and r.get('l3_device') == 'pon' and r.get('up') is True]
    security = {key: [int(v) for v in re.findall(r'^' + key + r'=(\d+)$', text, re.M)]
                for key in ('data_rx_key_valid', 'data_tx_key_index', 'security_keys_valid', 'data_key_pending')}
    key_installed = any(v != 0 for v in security['data_rx_key_valid'])
    result = dict(status='observed' if returncode == 0 and stages.get('cleanup') == 'passed' and pairs_valid else 'failed',
                  returncode=returncode, stages=stages, rx_samples=len(rx), diagnostics_pairs_valid=pairs_valid,
                  timing_errors=len(re.findall(r'^sleep: .+$', text, re.M)),
                  synchronized_samples=sum(r.get('synced') is True for r in rx),
                  rx_power_dbm_range=[round(min(power), 2), round(max(power), 2)] if power else None,
                  frames_first=rx[0].get('frames') if rx else None, frames_last=rx[-1].get('frames') if rx else None,
                  observed_states=sorted({r['state'] for r in omci}),
                  o5_authenticated=any(r['state'] == 5 and r.get('authenticated') == 1 for r in omci),
                  provisioned=stages.get('provisioning') == 'passed',
                  dhcp_ipv4=any(r.get('proto') == 'dhcp' and r.get('ipv4-address') for r in network),
                  dhcpv6_address=any(r.get('proto') == 'dhcpv6' and r.get('ipv6-address') for r in network),
                  dhcpv6_prefix=any(r.get('proto') == 'dhcpv6' and r.get('ipv6-prefix') for r in network),
                  traffic=traffic, security=security, downstream_key_installed=key_installed,
                  encrypted_traffic_proven=False,
                  encryption_note='Key state and traffic are separate evidence; encrypted GEM counter attribution is not exposed.',
                  throughput={direction: int(code) == 0 for direction, code in re.findall(
                      r'^throughput_result direction=(\S+) rc=(\d+)$', text, re.M)})
    tx_records = [r for r in records if isinstance(r,dict) and r.get('transmitter_version') == 1]
    mac = [r for r in live_records if isinstance(r,dict) and r.get('mac_version') == 2]
    phases = {}
    for phase, raw in re.findall(r'^tx_observation phase=(\S+)\n(\{[^\n]+\})', text, re.M):
        sample = json.loads(raw)
        if sample.get('transmitter_version') == 1: phases.setdefault(phase, []).append(sample)
    tx_fields = {}
    for phase, rows in phases.items():
        tx_fields[phase] = {}
        for name in ('bias','modulation','tx_power','bias_code','modulation_code','tx_control','ben_status','ocp_status'):
            values = [r['fields'][name]['value'] for r in rows if r['fields'].get(name,{}).get('valid') is True]
            tx_fields[phase][name] = dict(valid_samples=len(values), unavailable_samples=len(rows)-len(values),
                minimum=min(values) if values else None, maximum=max(values) if values else None,
                changed=len(set(values))>1)
    result['tx'] = dict(samples=len(tx_records), mac_samples=len(mac), by_phase=tx_fields,
        burst_segments=burst_segments(mac),
        burst_counter_first=mac[0]['registers'].get('5944') if mac else None,
        burst_counter_last=mac[-1]['registers'].get('5944') if mac else None,
        unavailable_records=sum(isinstance(r,dict) and r.get('transmitter_unavailable') is True for r in records),
        fast_transition_unavailable=sum(isinstance(r,dict) and r.get('fast_version') == 2 and r.get('available') is False for r in records),
        connector_emission_verified=False, sensor_refresh_verified=False,
        note='Internal DDMI/drive observations only. Constant or zero values do not establish no light; bursts can be missed and MCU sensor age is unknown. Counter resets must be separated before computing deltas.')
    if events is not None:
        disconnect = next((e['sampled_ms'] for e in events if e['action'] == 'DISCONNECTED'), None)
        reconnect = next((e['sampled_ms'] for e in events if e['action'] == 'RECONNECTED'), None)
        dark = [r for r in rx if disconnect is not None and reconnect is not None and disconnect < r['sampled_ms'] <= reconnect]
        after = [r for r in rx if reconnect is not None and r['sampled_ms'] > reconnect]
        result['physical_control'] = dict(events=events,
            confirmed=disconnect is not None and reconnect is not None,
            dark_samples=sum(r.get('controller_los') is True and r.get('phy_los') is True and r.get('synced') is False for r in dark),
            recovered_samples=sum(r.get('synced') is True for r in after))
        result['physical_control']['passed'] = bool(disconnect is not None and reconnect is not None
            and result['physical_control']['dark_samples'] >= 15 and result['physical_control']['recovered_samples'] >= 15)
        if not result['physical_control']['passed']:
            result['status'] = 'failed'
    result['trace'] = trace_summary(records)
    result['registration'] = registration_summary(records)
    result['omci_experiment'] = omci_summary(records)
    result['interface_counters'] = [r for r in records if isinstance(r,dict) and r.get('interface_version')==1]
    winners = re.findall(r'^recovery_winner=([1-7])$', text, re.M)
    result['recovery_winner'] = winners[-1] if winners else None
    result['recovery_sequence'] = re.findall(r'^recovery_action=([1-7]) phase=applied$', text, re.M)
    result['security'] = {key:dict(first=values[0] if values else None, last=values[-1] if values else None,
                                  nonzero_samples=sum(v != 0 for v in values), samples=len(values)) for key,values in security.items()}
    return result


def redactor(identity):
    patterns = set()
    for key in ('serial', 'registration_id', 'logical_onu_id', 'logical_password', 'wan_mac'):
        value = identity.get(key, '')
        if value:
            patterns.add(value)
            patterns.add(value.encode().hex())
            if key == 'registration_id':
                patterns.add(value.ljust(72, '0'))
            if key == 'serial':
                patterns.add(value[:4].encode().hex() + value[4:])
    def redact(text):
        for value in sorted(patterns, key=len, reverse=True):
            # Short identifiers may match counters. Restrict those to log lines
            # explicitly naming credentials; never corrupt structured evidence.
            if len(value) >= 8:
                text = re.sub(re.escape(value), '<private>', text, flags=re.I)
        text = re.sub(r'^.*(?:pon_reg_id=|logical_password=|registration_id=).*$', '<private-credential-line>', text, flags=re.M)
        return text
    return redact


def capture(pin, case, iperf, directory, redact):
    name = case['name']
    timing = ''
    if TIMING_SLEEP is not None:
        if digest(TIMING_SLEEP) != pin.get('timing_sleep_sha256'):
            raise ValueError('Timing helper differs from its pinned hash')
        timing = 'test -x ' + STAGE + '/sleep\n'
        timing += "sha256sum -c <<'TIMING_SUM'\n" + digest(TIMING_SLEEP) + '  ' + STAGE + "/sleep\nTIMING_SUM\n"
        timing += 'PATH=' + STAGE + ':"$PATH"; export PATH\n'
    script = guards(pin) + timing + 'umask 077; printf "%s\\n" "$$" > ' + STAGE + '/helper.pid\nexec q1000k-pon-validate ' + shlex.join([
        case['mode'], STAGE + '/xgspon-calibration.bin', STAGE, str(case['samples']), iperf, name, case.get('recovery_actions','1,2,3,4,5,6,7')]) + '\n'
    physical = name in PHYSICAL
    events, samples, lines = [], [], []
    trace_seen = set(); stack_generation = 0; in_postmortem = False
    dark_start = name == 'rx-dark-start'
    if dark_start:
        print('Disconnect fiber before initialization, then type DISCONNECTED.', flush=True)
        if input().strip() != 'DISCONNECTED': raise ValueError('Dark initialization needs explicit confirmation')
        dark_confirmed_at = time.time()
    deadline = time.monotonic() + (1500 if physical else case['samples'] * (8 if case['mode'] == 'activate' else 3) + 180)
    process = subprocess.Popen(SSH + ['sh', '-s'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, bufsize=0, start_new_session=True)
    process.stdin.write(script.encode()); process.stdin.close(); process.stdin = None
    waiting = None; disconnected = reconnected = False; pending = b''; last_notice = time.monotonic()
    print(f'{name}: collecting; TX {"permitted" if case["mode"] in ("activate", "isolated") else "inhibited"}.', flush=True)
    try:
        with (directory / (name + '.log')).open('w') as output:
            while True:
                if time.monotonic() > deadline:
                    raise TimeoutError('Bounded capture exceeded its deadline')
                sources = [process.stdout] + ([sys.stdin] if waiting else [])
                ready, _, _ = select.select(sources, [], [], 1)
                if waiting and sys.stdin in ready:
                    answer = sys.stdin.readline().strip()
                    if answer == waiting:
                        events.append(dict(action=answer, sampled_ms=samples[-1]['sampled_ms'], confirmed_at=time.time()))
                        ssh('set -eu; test -d /var/run/q1000k-pon-bench.lock; printf ' + shlex.quote(answer+'\n') + ' > /var/run/q1000k-pon-bench.lock/control', timeout=15)
                        disconnected |= answer == 'DISCONNECTED'; reconnected |= answer == 'RECONNECTED'; waiting = None
                    elif not answer and not sys.stdin.isatty():
                        raise RuntimeError('Physical control input ended')
                if process.stdout in ready:
                    data = os.read(process.stdout.fileno(), 65536)
                    if not data:
                        if pending:
                            line = pending.decode(errors='replace'); lines.append(line); output.write(redact(line))
                        break
                    pending += data
                    while b'\n' in pending:
                        chunk, pending = pending.split(b'\n', 1)
                        line = chunk.decode(errors='replace') + '\n'
                        if line.startswith('stack_generation='):
                            stack_generation = int(line.split('=')[1]); trace_seen.clear()
                        if line.strip() == 'postmortem_begin': in_postmortem = True
                        elif line.strip() == 'postmortem_end': in_postmortem = False
                        try: record = json.loads(line)
                        except ValueError: record = None
                        if isinstance(record, dict) and record.get('mac_version') == 2:
                            record['stack_generation'] = stack_generation
                            line = json.dumps(record)+'\n'
                        if isinstance(record, dict) and any(k.startswith(('trace_','critical_')) for k in record):
                            record['stack_generation'] = stack_generation
                            line = json.dumps(record)+'\n'
                        if isinstance(record, dict) and (record.get('trace_version') == 1 or record.get('critical_version') == 1):
                            key = (stack_generation, record['seq'], record.get('first',False), record.get('critical_version',0))
                            if key in trace_seen: continue
                            trace_seen.add(key)
                            record['stack_generation'] = stack_generation
                            line = json.dumps(record)+'\n'
                        lines.append(line); output.write(redact(line)); output.flush()
                        if isinstance(record, dict) and 'receiver_version' in record and not in_postmortem:
                            samples.append(record)
                            if dark_start and not disconnected:
                                events.append(dict(action='DISCONNECTED', sampled_ms=record['sampled_ms']-1, confirmed_at=dark_confirmed_at, before_init=True))
                                disconnected = True
                            if len(samples) == 1 and record.get('rx_power_valid') and record.get('rx_power_nw', 0) > 0:
                                print(f"RX power: {10*math.log10(record['rx_power_nw']/1000000):.2f} dBm; LOS={record.get('controller_los')}, sync={record.get('synced')}.", flush=True)
                if physical and not waiting and samples:
                    if not disconnected and len(samples) >= 15 and all(r.get('synced') for r in samples[-15:]):
                        waiting = 'DISCONNECTED'
                        print('Disconnect fiber now, then type DISCONNECTED. Capture continues.', flush=True)
                    elif disconnected and not reconnected and len(samples) >= (dark_min := 3 if name == 'rx-short-outage' else 30 if name == 'rx-long-outage' else 15) and all(
                            r.get('controller_los') is True and r.get('phy_los') is True and not r.get('synced')
                            and r['sampled_ms'] > events[-1]['sampled_ms'] for r in samples[-dark_min:]):
                        waiting = 'RECONNECTED'
                        print('Dark control recorded. Reconnect fiber, then type RECONNECTED. Capture continues.', flush=True)
                if time.monotonic() - last_notice >= 15:
                    print(f'{name}: {len(samples)} coherent samples captured.', flush=True); last_notice = time.monotonic()
            code = process.wait(timeout=20)
    except BaseException:
        # Keep the output transport open while the remote EXIT trap exports
        # history and drains hardware. Closing it first can kill the trap with
        # SIGPIPE before stop_stack. The child SSH is isolated from Ctrl-C.
        try:
            if process.poll() is None:
                ssh('set -eu; if [ -f ' + STAGE + '/helper.pid ]; then '
                    'read -r p < ' + STAGE + '/helper.pid; '
                    'case "$p" in ""|*[!0-9]*) exit 1;; esac; '
                    'if [ -r /proc/$p/cmdline ]; then '
                    'test "$(tr "\\000" "\\n" < /proc/$p/cmdline | sed -n "2p")" = /usr/sbin/q1000k-pon-validate; '
                    'kill -TERM "$p"; fi; fi', timeout=15)
            remaining, _ = process.communicate(timeout=45)
            if pending or remaining:
                with (directory / (name + '.log')).open('a') as output:
                    output.write(redact((pending + (remaining or b'')).decode(errors='replace')))
        finally:
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=15)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
        raise
    result = summarize(''.join(lines), code, events if physical else None)
    result['name'] = name
    result['required_dark_samples'] = 3 if name == 'rx-short-outage' else 30 if name == 'rx-long-outage' else 15
    result['trace_scope'] = 'activation-critical' if name.startswith('activation-reg-') else 'all-events'
    result['status'] = case_outcome(result, critical_only=name.startswith(('activation-reg-', 'activation-omci-')))
    if case['mode'] == 'isolated':
        rows = [json.loads(line) for line in lines if line.startswith('{"isolated_tx_version":')]
        result['isolated'] = rows[-1] if rows else {}
        sample = result['isolated']
        clean = (result['returncode'] == 0 and result['stages'].get('cleanup') == 'passed'
                 and sample.get('restored') is True and sample.get('tx_off') is True
                 and sample.get('id') == int(name.split('-')[1]))
        valid = sample.get('valid_phases', 0)
        unavailable = sample.get('error') == -61 and valid == 5
        measured = sample.get('error') == 0 and valid == 7 and sample.get('dark_checks', 0) >= 3
        result['status'] = ('unavailable-calibration' if unavailable else 'observed') if clean and (unavailable or measured) else 'containment-failure'
        result['planned_ids'] = case.get('ids', [])
        result['test_label'] = case['label']
        result['test_outcomes'] = {i:result['status'] for i in case.get('ids', [])}
        # Label internal measurements by the kernel's test window, including
        # the generator-with-TX-disabled negative control. enabled_ns stays
        # zero for that control and must never be treated as TX enabled.
        # Fresh I2C timestamps still do not establish DDMI refresh/age.
        begin, end = sample.get('window_ns',0), sample.get('disabled_ns',0)
        tx_rows = []
        for line in lines:
            try: row = json.loads(line)
            except ValueError: continue
            if isinstance(row,dict) and row.get('transmitter_version') == 1: tx_rows.append(row)
        result['tx']['by_window'] = {}
        for phase in ('before','active','after'):
            aligned = [r for r in tx_rows if (phase == 'before' and r['end_ns'] <= begin or
                      phase == 'active' and begin and r['begin_ns'] >= begin and r['end_ns'] <= end or
                      phase == 'after' and r['begin_ns'] >= end)] if end else []
            result['tx']['by_window'][phase] = {}
            for field in ('bias','modulation','tx_power','bias_code','modulation_code','ocp_status','ben_status'):
                values = [r['fields'][field]['value'] for r in aligned if r['fields'].get(field,{}).get('valid') is True]
                result['tx']['by_window'][phase][field] = dict(samples=len(values), minimum=min(values) if values else None,
                    maximum=max(values) if values else None, changed=len(set(values)) > 1)
        if int(name.split('-')[1]) >= 32:
            result['output'], result['output_outcome'] = output_result(
                [json.loads(line) for line in lines if line.startswith('{\"output_version\":')], int(name.split('-')[1]), sample)
            if result['output_outcome'] == 'containment-failure': result['status'] = 'containment-failure'
        elif int(name.split('-')[1]) >= 19:
            result['mpd'], result['measurement_outcome'] = monitor_result(
                [json.loads(line) for line in lines if line.startswith('{"mpd_version":')], int(name.split('-')[1]))
            if result['measurement_outcome'] == 'containment-failure': result['status'] = 'containment-failure'
            # Retain monitor intervals so downstream analysis can exclude perturbations.
            result['tx']['active_monitor_intervals_ns'] = [
                [p['begin_ns'], p['end_ns']] for p in result['mpd'].get('probes', []) if p.get('active')]
        return result
    if result['rx_samples'] < 15 and result['status'] != 'containment-failure': result['status'] = 'inconclusive'
    result['planned_ids'] = case.get('ids', [])
    result['independent_recovery'] = name == 'rx-confirm' and len(result['recovery_sequence']) == 1 and bool(result['recovery_winner'])
    if case['mode'] == 'rx' and not physical and result['returncode'] == 0 and result['rx_samples'] != case['samples']:
        result.update(status='failed', sample_count_error=True)
    result['test_outcomes'] = test_outcomes(case, result, ''.join(lines))
    return result


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n'); path.chmod(0o600)


def execute(args, pin):
    if not args.inputs or not args.output:
        raise ValueError('Collection requires --inputs and --output')
    prior = json.loads(args.resume.read_text()) if getattr(args,'resume',None) else None
    supplied_identity = json.loads(args.identity.read_text()) if args.identity else {}
    identity = validate_identity(supplied_identity) if args.identity else {}
    rx_only = args.rx_only or args.physical_only or not identity
    cases = discovery_plan(args)
    payload = private_inputs(args.inputs, identity)
    if any(c['name'] in PHYSICAL for c in cases) and not sys.stdin.isatty():
        raise ValueError('Use an interactive terminal for physical controls, or --skip-physical to record them as not run')
    serial_stat = args.serial_log.stat()
    if not args.serial_log.is_file():
        raise ValueError('Serial log must be a regular file')
    args.output.mkdir(mode=0o700)
    redact = redactor(identity)
    record = dict(schema_version=1, artifact={k:v for k,v in pin.items() if k != 'runtime'},
                  started=time.time(), cases=cases, results=[], status='running',
                  activation_requested=not rx_only, physical_skipped=not any(c['name'] in PHYSICAL for c in cases),
                  physical_only=args.physical_only,
                  registration_source=('explicit' if supplied_identity.get('registration_id') else 'zero-default') if not rx_only else 'unused',
                  private_payloads_included=False, hardware_service_verified=False)
    record['input_archive_sha256'] = digest(args.inputs.read_bytes())
    record['identity_sha256'] = digest(json.dumps(identity,sort_keys=True).encode())
    staged = False
    with os.fdopen(os.open(Path(tempfile.gettempdir()) / ('q1000k-bench-' + HOST + '.lock'),
                          os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW, 0o600), 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            (args.output/'preflight.log').write_text(ssh(guards(pin)))
            record['boot_id'] = ssh('cat /proc/sys/kernel/random/boot_id').strip()
            if prior:
                if (prior.get('artifact') != record['artifact'] or prior.get('boot_id') != record['boot_id'] or
                    not prior.get('cleanup_verified') or any(prior.get(k) != record[k] for k in ('input_archive_sha256','identity_sha256'))):
                    raise ValueError('Resume requires the same pinned image, boot, inputs and verified idle cleanup')
                record['resumed_from'] = str(args.resume)
                prior_cases = {c['name']:c for c in prior.get('cases',[])}
                if any(c['name'] in prior_cases and c != prior_cases[c['name']] for c in cases):
                    raise ValueError('Resume case settings differ from the saved collection')
                completed = {r['name'] for r in prior['results'] if r['status'] in ('observed','functional-negative')}
                record['results'] = [r for r in prior['results'] if r['name'] in completed]
                cases = [c for c in cases if c['name'] not in completed]
                # Only whole finished sessions are reused. Never reconstruct a
                # failed receiver state from a checkpoint or resume mid-action.
            ssh(f'set -eu; umask 077; test ! -e {STAGE}; test ! -e {FIRMWARE}; mkdir {STAGE}')
            staged = True
            ssh(f'set -eu; tar -x -C {STAGE}; cd {STAGE}; sha256sum -c sha256sums >/dev/null; mkdir -p {FIRMWARE}; cp A60993.elf.pm A60993.elf.dm {FIRMWARE}/', payload=payload)
            for case in cases:
                if case.get('requires_winner'):
                    prior = next((r for r in record['results'] if r['name']=='rx-reconnect'), {})
                    if not prior.get('recovery_winner') or prior.get('status') != 'observed':
                        record['results'].append(dict(name=case['name'], status='skipped', reason='No valid winning action to reproduce')); continue
                    case['recovery_actions'] = prior['recovery_winner']
                if case.get('requires_sn_reset') and not any(r.get('status') != 'containment-failure' and r.get('stages',{}).get('cleanup') == 'passed' and r.get('trace',{}).get('sn_threshold_reset') for r in record['results']):
                    record['results'].append(dict(name=case['name'], status='skipped', reason='SN threshold reset not observed')); continue
                result = capture(pin, case, args.iperf_server or 'none', args.output, redact)
                record['results'].append(result)
                write_json(args.output/'collection.json', record)
                (args.output/(case['name']+'-postflight.log')).write_text(ssh(guards(pin)))
                if result['status'] == 'containment-failure':
                    raise RuntimeError(case['name'] + ' did not meet its capture/cleanup checks')
            record['status'] = 'collection-complete'
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired, KeyboardInterrupt) as error:
            record.update(status='stopped', error=redact(str(error) or type(error).__name__))
        finally:
            try:
                # Never remove the inputs beneath an active controller lease.
                (args.output/'postflight.log').write_text(ssh(guards(pin)))
                if staged:
                    ssh(f'set -eu; rm -f {FIRMWARE}/A60993.elf.pm {FIRMWARE}/A60993.elf.dm; rmdir {FIRMWARE} 2>/dev/null || test ! -e {FIRMWARE}; rm -f ' + ' '.join(STAGE+'/'+n for n in (*INPUTS, 'identity.json', 'sha256sums', 'helper.pid', *(['sleep'] if TIMING_SLEEP is not None else []))) + f'; rmdir {STAGE}')
                record['cleanup_verified'] = True
            except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
                record.update(status='stopped', cleanup_verified=False, cleanup_error=redact(str(error)))
            try:
                after = args.serial_log.stat()
                if after.st_ino != serial_stat.st_ino or after.st_size < serial_stat.st_size:
                    raise ValueError('Serial log rotated/truncated during collection')
                with args.serial_log.open('rb') as stream:
                    stream.seek(serial_stat.st_size)
                    (args.output/'serial.log').write_text(redact(stream.read().decode(errors='replace')))
            except (OSError, ValueError) as error:
                record.update(status='stopped', serial_error=str(error))
            record['finished'] = time.time()
            record['not_run'] = [c['name'] for c in cases if c['name'] not in {r['name'] for r in record['results']}]
            if args.physical_only:
                record['not_run'] += ['rx-startup', 'rx-repeat-1', 'rx-repeat-2', 'rx-soak']
            if getattr(args, 'suite', 'legacy') in ('isolated', 'measurement', 'output'):
                record['not_run'] += ['OLT discovery', 'serial acceptance', 'ranging', 'O5', 'OMCI provisioning', 'WAN traffic']
                record['identity_note'] = 'Disconnected PHY-only suite; no identity programmed and no registration executor loaded.'
            elif rx_only:
                record['not_run'] += ['activation', 'provisioning', 'wan', 'traffic']
                record['identity_note'] = 'Activation omitted: RX-only selected or no private identity supplied.'
            active = next((r for r in record['results'] if r.get('provisioned')), next(
                (r for r in record['results'] if r.get('name','').startswith('activation')), {}))
            record['hardware_service_verified'] = bool(record.get('cleanup_verified') and active.get('status') == 'observed' and active.get('provisioned') and
                (active.get('dhcp_ipv4') and active.get('traffic', {}).get('ipv4_https') or
                 active.get('dhcpv6_address') and active.get('traffic', {}).get('ipv6_https')))
            if not rx_only:
                for stage in ('provisioning', 'wan', 'traffic-soak'):
                    if stage not in active.get('stages', {}):
                        record['not_run'].append(stage)
            write_json(args.output/'collection.json', record)
            if getattr(args, 'suite', 'legacy') in ('omci', 'continuity', 'topology'):
                report = ['# OMCI runt and reconfiguration observations', '',
                    '| Case | Runt OMCI admitted | Authenticated RX | Replies queued | TX auth errors | Allocation sessions kept | Deferred / consumed | Drops | Evidence complete |',
                    '|---|---:|---:|---:|---:|---:|---|---:|---|']
                for result in record['results']:
                    o = result.get('omci_experiment', {})
                    report.append(f"| {result['name']} | {o.get('ethernet_runt_omci_delivered',0)} | {o.get('authenticated_rx',0)} | {o.get('replies_queued',0)} | {o.get('tx_auth_rejected',0)} | {o.get('allocations_kept_session',0)} | {o.get('replies_deferred',0)} / {o.get('deferred_native_consumed',0)} | {o.get('reply_drops',0)} | {o.get('critical_evidence_complete',False)} |")
                report += ['', '## Native transmit and live additions', '',
                    '| Case | Submitted | DMA complete | HW drop | Admission drop | DMA error | Abort | Retry | Live adds | Evidence complete |',
                    '|---|---:|---:|---:|---:|---:|---:|---:|---:|---|']
                for result in record['results']:
                    o=result.get('omci_experiment',{}); n=o.get('native_tx',{}).get('counts',{})
                    report.append(f"| {result['name']} | {n.get('submitted',0)} | {n.get('dma-complete',0)} | {n.get('hardware-drop',0)} | {n.get('admission-drop',0)} | {n.get('dma-error',0)} | {n.get('teardown-abort',0)} | {n.get('admission-retry',0)} | {o.get('live_additions',{}).get('completed',0)} | {o.get('critical_evidence_complete',False)} |")
                report += ['', 'Native consumption does not prove upstream optical delivery. An unobserved allocation/pause is an untested condition; use the raw logs and collection.json to distinguish it from success.']
                report += ['', '## Managed-entity responses', '',
                    'Counts include duplicate replies and are limited to retained evidence. Details and provider errno/stage are in collection.json.', '',
                    '| Case | Class | Entity | Opcode | Mask | Result | Count | Duplicates |',
                    '|---|---:|---|---:|---|---:|---:|---:|']
                for result in record['results']:
                    for row in result.get('omci_experiment', {}).get('provisioning', {}).get('response_results', []):
                        entity = 'unknown' if row['entity_id'] is None else f"0x{row['entity_id']:04x}"
                        mask = 'unknown' if row['attribute_mask'] is None else f"0x{row['attribute_mask']:04x}"
                        report.append(f"| {result['name']} | {row['class_id']} | {entity} | {row['opcode']} | {mask} | {row['result']} | {row['count']} | {row['duplicates']} |")
                (args.output/'omci-summary.md').write_text('\n'.join(report)+'\n')
            if getattr(args, 'suite', 'legacy') == 'registration':
                report = ['# Registration hypotheses 1–5', '',
                    'Host processing times and internal transmission evidence. Enqueue is not optical delivery or OLT acceptance.', '',
                    '| Case | Collection result | Ranging → ACK enqueue ms | Key request → enqueue ms | Key reports | OLT deactivations | Complete snapshots | Critical evidence complete |',
                    '|---|---|---|---|---:|---:|---:|---|']
                def span(values):
                    return f'{min(values):.3f}–{max(values):.3f} ({len(values)})' if values else 'not observed'
                for result in record['results']:
                    a = result.get('registration', {})
                    report.append(f"| {result['name']} | {result['status']} | {span(a.get('ranging_to_ack_ms', []))} | {span(a.get('key_request_to_enqueue_ms', []))} | {a.get('key_reports_enqueued', 0)} | {a.get('olt_deactivations', 0)} | {a.get('complete_snapshots', 0)} | {a.get('critical_evidence_complete', False)} |")
                report += ['', 'Per-hypothesis coverage and raw boundary snapshots are in collection.json. A missed prerequisite is reported as not reached. Full service requires OMCI provisioning and successful traffic probes.']
                (args.output/'registration-summary.md').write_text('\n'.join(report)+'\n')
            if getattr(args, 'suite', 'legacy') in ('isolated', 'measurement', 'output'):
                report = ['# Disconnected transmitter observations', '',
                    'Internal sensor readings; connector emission and O5 remain unverified.', '',
                    '| Case | Result | Internal TX nW before / active / after | Bias uA before / active / after | TX off / restored |',
                    '|---|---|---|---|---|']
                def ranges(result, field):
                    values=[]
                    for phase in ('before','active','after'):
                        v=result.get('tx',{}).get('by_window',{}).get(phase,{}).get(field,{})
                        values.append(f"{v['minimum']}..{v['maximum']} ({v['samples']} samples)" if v.get('samples') else 'unavailable')
                    return ' / '.join(values)
                for result in record['results']:
                    iso=result.get('isolated',{})
                    report.append(f"| {result['name']} {result.get('test_label','')} | {result['status']} | {ranges(result,'tx_power')} | {ranges(result,'bias')} | {iso.get('tx_off')} / {iso.get('restored')} |")
                if args.suite == 'measurement':
                    report += ['', '## OEM monitor observations', '',
                        'Raw ADC/TSSI codes, with TX off / test active / restored. Conversion freshness and connector emission remain unverified.', '',
                        '| Case | Measurement result | Raw monitor by TX phase | Raw TSSI by TX phase |',
                        '|---|---|---|---|']
                    for result in record['results']:
                        m = result.get('mpd', {})
                        report.append(f"| {result['name']} {result.get('test_label','')} | {result.get('measurement_outcome','missing')} | {m.get('raw_monitor_by_phase')} | {m.get('raw_tssi_by_phase')} |")
                if args.suite == 'output':
                    report += ['', '## Gate and sensor correlation', '',
                        'Each cell lists off / on / off. Hardware TSSI is CSR 0x3a4 shifted by seven and truncated to 16 bits. Monitor current uses the NAND OEM lookup, only with verified monitor selection. Conversion age and connector output remain unverified.', '',
                        '| Case | Gate result | BEN | Hardware TSSI | Mailbox TSSI | MCU status 0xfe | Monitor current uA | Settings unchanged |',
                        '|---|---|---|---|---|---|---|---|']
                    for result in record['results']:
                        o = result.get('output', {})
                        def phases(key): return ' / '.join(str(p.get(key)) for p in o.get('by_phase', []))
                        report.append(f"| {result['name']} {result.get('test_label','')} | {result.get('output_outcome','missing')} | {phases('ben')} | {phases('hardware_tssi')} | {phases('mailbox_tssi')} | {phases('reporting_status')} | {phases('monitor_current_uA_oem')} | {o.get('fixed_settings_verified')} |")
                (args.output/'isolated-summary.md').write_text('\n'.join(report)+'\n')
            files = sorted(p for p in args.output.iterdir() if p.is_file())
            (args.output/'sha256sums').write_text(''.join(f'{digest(p.read_bytes())}  {p.name}\n' for p in files))
            archive = args.output.with_suffix('.tar.gz')
            with archive.open('xb') as stream, tarfile.open(fileobj=stream, mode='w:gz') as tar:
                for path in files + [args.output/'sha256sums']:
                    tar.add(path, arcname=args.output.name+'/'+path.name, recursive=False)
            archive.chmod(0o600)
    print(json.dumps(record, indent=2))
    print('Evidence archive:', archive)
    return 0 if record['status'] == 'collection-complete' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-single-file', type=Path)
    parser.add_argument('--timing-helper', type=Path, help='Embed the built AArch64 bench-sleep shim when generating a portable collector')
    parser.add_argument('--artifact', type=Path)
    parser.add_argument('--inputs', type=Path)
    parser.add_argument('--identity', type=Path, help='Private subscriber JSON; permits TX activation after RX checks')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--serial-log', type=Path, default=Path('/tmp/serial_output.log'))
    parser.add_argument('--suite', choices=('topology','continuity','omci','registration','output','measurement','isolated','tx','legacy'), default='topology', help='Default: RX control, complete MIB topology, strict/factory Dot1X, OEM EqD and repeat; no fiber cycling. continuity selects earlier live-addition controls. omci includes earlier runt/revocation controls. registration selects the earlier hypotheses 1–5. output/measurement/isolated select disconnected tests; tx selects earlier discovery comparisons; legacy selects recovery tests')
    parser.add_argument('--rx-only', action='store_true')
    parser.add_argument('--skip-physical', action='store_true')
    parser.add_argument('--physical-only', action='store_true', help='Run only the TX-inhibited disconnect/reconnect control')
    parser.add_argument('--resume', type=Path, help='Continue independent sessions from a collection.json on the same idle boot')
    parser.add_argument('--cases', help='Comma-separated session names from --dry-run; each begins with fresh ownership')
    parser.add_argument('--recovery-action', choices=tuple('1234567'), help='Independently select one fixed recovery action in the same image')
    parser.add_argument('--soak', type=int, choices=(180, 300, 600), default=180)
    parser.add_argument('--iperf-server', help='Optional reachable numeric iperf3 server address')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--summarize', type=Path, help='Recheck one existing stage log without device access')
    args = parser.parse_args()
    if args.iperf_server:
        args.iperf_server = str(ipaddress.ip_address(args.iperf_server))
    if args.build_single_file:
        if not args.artifact: parser.error('--build-single-file requires --artifact')
        return build_single_file(args.artifact, args.build_single_file, args.timing_helper)
    if args.timing_helper:
        parser.error('--timing-helper requires --build-single-file')
    if args.summarize:
        print(json.dumps(summarize(args.summarize.read_text()), indent=2)); return 0
    pin = PIN if PIN is not None else artifact_pin(args.artifact) if args.artifact else None
    if not pin:
        parser.error('Source collector requires --artifact; generated copy embeds its pin')
    runtime_manifest(pin['runtime'])
    if args.dry_run:
        print(json.dumps(dict(artifact={k:v for k,v in pin.items() if k != 'runtime'},
                             cases=discovery_plan(args),
                             activation_requires_private_identity=True), indent=2)); return 0
    return execute(args, pin)


if __name__ == '__main__':
    os.umask(0o077)
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.TimeoutExpired) as error:
        print('Collection stopped:', error, file=sys.stderr)
        raise SystemExit(1)
