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
    files['identity.json'] = (json.dumps(identity) + '\n').encode()
    files['sha256sums'] = ''.join(f'{digest(data)}  {name}\n' for name, data in files.items()).encode()
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w') as archive:
        for name, data in files.items():
            member = tarfile.TarInfo(name)
            member.size, member.mode = len(data), 0o600
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


def build_single_file(artifact, output):
    if PIN is not None:
        raise ValueError('Generate from the source collector')
    pin = artifact_pin(artifact)
    marker = 'PIN = ' + 'None\n'
    source = Path(__file__).read_text()
    if source.count(marker) != 1:
        raise ValueError('Ambiguous embedding marker')
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
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode:
        raise RuntimeError('Device command failed: ' + result.stdout.decode(errors='replace')[-4000:])
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

def discovery_plan(args):
    if args.physical_only and args.skip_physical:
        raise ValueError('--physical-only cannot be combined with --skip-physical')
    rx_only = args.rx_only or args.physical_only or not args.identity
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
        case['recovery_actions'] = getattr(args, 'recovery_action', None) or '1,2,3,4,5,6,7'
        if case.get('requires_winner') and getattr(args, 'recovery_action', None):
            case.pop('requires_winner')
    return cases


def trace_summary(records):
    events = [r for r in records if isinstance(r,dict) and r.get('trace_version') == 1 and not r.get('first')]
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
        profile_commit_observed=any(r['event']==14 and r['id']==1 and r['b'] and r['c'] for r in events),
        sn_request_interrupts=counter(8,2),sn_sent_interrupts=counter(8,3),
        ranging_request_interrupts=counter(8,4),registration_sent_interrupts=counter(8,5),
        local_assignment_observed=any(r['event']==15 and r['a']==1 for r in events),
        ranging_accepted=counter(11,4)-counts.get('11:4',{}).get('errors',0))
    return dict(milestones=milestones, events=sum(map(len,groups.values())), counters=counts, internal_sequence_gaps=gaps,
                concurrent_wrap=any(isinstance(r,dict) and 'trace_gap' in r for r in records),
                sn_threshold_reset=any(r['event']==16 and r['id']==1 for r in events),
                first_reset=next((r for r in events if r['event']==16), None),
                first_fault=next((r for r in events if r['event']==2), None),
                generation_changes=sorted({r['generation'] for r in events}))


def test_outcomes(case, result, text):
    """Keep selectable capabilities separate from branches actually observed."""
    stages = result['stages']; outcome = {}
    for ident in case.get('ids', []):
        outcome[ident] = 'not-run'
        if ident in ('B02','B03'): outcome[ident] = result['status']
        elif ident in ('A01','A02','A03','D01') and 'activation' in stages:
            outcome[ident] = stages['activation']
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


def case_outcome(result, trace_required=True):
    # A functional negative is useful evidence; containment cannot be waived.
    if result['stages'].get('cleanup') != 'passed' or result['stages'].get('failure') == 'containment':
        return 'containment-failure'
    physical = result.get('physical_control')
    if not result['diagnostics_pairs_valid'] or (physical and (not physical['confirmed'] or physical.get('dark_samples',0) < result.get('required_dark_samples',15))):
        return 'inconclusive'
    t = result.get('trace', {})
    if trace_required and (not t.get('events') or t.get('internal_sequence_gaps') or t.get('concurrent_wrap')):
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
            value, length = decoder.raw_decode(text[offset:])
            values.append(value); offset += length
        except ValueError:
            offset += 1
    return values


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
    script = guards(pin) + 'exec q1000k-pon-validate ' + shlex.join([
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
                               stderr=subprocess.STDOUT, bufsize=0)
    process.stdin.write(script.encode()); process.stdin.close()
    waiting = None; disconnected = reconnected = False; pending = b''; last_notice = time.monotonic()
    print(f'{name}: collecting; TX {"permitted" if case["mode"] == "activate" else "inhibited"}.', flush=True)
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
                        if isinstance(record, dict) and any(k.startswith('trace_') for k in record):
                            record['stack_generation'] = stack_generation
                            line = json.dumps(record)+'\n'
                        if isinstance(record, dict) and record.get('trace_version') == 1:
                            key = (stack_generation, record['seq'], record.get('first',False))
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
        # HUP closes the remote shell/exec helper; the helper's EXIT trap owns
        # optical teardown. Postflight below must verify it before more work.
        process.terminate()
        try: process.wait(timeout=15)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        raise
    result = summarize(''.join(lines), code, events if physical else None)
    result['name'] = name
    result['required_dark_samples'] = 3 if name == 'rx-short-outage' else 30 if name == 'rx-long-outage' else 15
    result['status'] = case_outcome(result)
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
                  activation_requested=not rx_only, physical_skipped=args.skip_physical,
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
                if case.get('requires_sn_reset') and not any(r.get('status') in ('observed','functional-negative') and r.get('trace',{}).get('sn_threshold_reset') for r in record['results']):
                    record['results'].append(dict(name=case['name'], status='skipped', reason='SN threshold reset not observed')); continue
                result = capture(pin, case, args.iperf_server or 'none', args.output, redact)
                record['results'].append(result)
                write_json(args.output/'collection.json', record)
                (args.output/(case['name']+'-postflight.log')).write_text(ssh(guards(pin)))
                if result['status'] == 'containment-failure':
                    raise RuntimeError(case['name'] + ' did not meet its capture/cleanup checks')
            record['status'] = 'collection-complete'
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired, KeyboardInterrupt) as error:
            record.update(status='stopped', error=redact(str(error)))
        finally:
            try:
                # Never remove the inputs beneath an active controller lease.
                (args.output/'postflight.log').write_text(ssh(guards(pin)))
                if staged:
                    ssh(f'set -eu; rm -f {FIRMWARE}/A60993.elf.pm {FIRMWARE}/A60993.elf.dm; rmdir {FIRMWARE} 2>/dev/null || test ! -e {FIRMWARE}; rm -f ' + ' '.join(STAGE+'/'+n for n in (*INPUTS, 'identity.json', 'sha256sums')) + f'; rmdir {STAGE}')
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
            if rx_only:
                record['not_run'] += ['activation', 'provisioning', 'wan', 'traffic']
                record['identity_note'] = 'Activation omitted: RX-only selected or no private identity supplied.'
            active = next((r for r in record['results'] if r['name'] == 'activation'), {})
            record['hardware_service_verified'] = bool(record.get('cleanup_verified') and active.get('status') == 'observed' and active.get('provisioned') and
                (active.get('dhcp_ipv4') and active.get('traffic', {}).get('ipv4_https') or
                 active.get('dhcpv6_address') and active.get('traffic', {}).get('ipv6_https')))
            if not rx_only:
                for stage in ('provisioning', 'wan', 'traffic-soak'):
                    if stage not in active.get('stages', {}):
                        record['not_run'].append(stage)
            write_json(args.output/'collection.json', record)
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
    parser.add_argument('--artifact', type=Path)
    parser.add_argument('--inputs', type=Path)
    parser.add_argument('--identity', type=Path, help='Private subscriber JSON; permits TX activation after RX checks')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--serial-log', type=Path, default=Path('/tmp/serial_output.log'))
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
        return build_single_file(args.artifact, args.build_single_file)
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
