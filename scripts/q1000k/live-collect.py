#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Observe an existing PON stack without taking its lifecycle or network ownership.

Uses Python 3 and OpenSSH on the workstation. No router-side installation,
configuration, module loading, service restart or cleanup is performed.
Optional ping probes use the existing PON interface and are explicitly selected.
"""
import argparse
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time


SNAPSHOT_SCRIPT = r'''export LC_ALL=C
. /usr/share/libubox/jshn.sh || exit 1
emit() {
    local name="$1" data rc
    shift
    data=$("$@" 2>/dev/null); rc=$?
    json_init
    json_add_string name "$name"
    json_add_int returncode "$rc"
    json_add_string data "$data"
    json_dump
}
emit board cat /tmp/sysinfo/board_name
emit boot_id cat /proc/sys/kernel/random/boot_id
emit uptime cat /proc/uptime
emit build_info cat /build_info
emit status /usr/sbin/q1000k-xgspon status
emit supervisor ubus call service list '{"name":"q1000k-xgspon"}'
emit pon_link ip -j -s link show dev pon
emit ponraw_link ip -j -s link show dev ponraw
emit addresses ip -j address show dev pon
emit routes4 ip -4 -j route show table all dev pon
emit routes6 ip -6 -j route show table all dev pon
emit wan4 ubus call network.interface.q1000k_wan status
emit wan6 ubus call network.interface.q1000k_wan6 status
'''
TEXT_FIELDS = {'board', 'boot_id', 'uptime', 'build_info'}
FIELDS = TEXT_FIELDS | {'status', 'supervisor', 'pon_link', 'ponraw_link',
                        'addresses', 'routes4', 'routes6', 'wan4', 'wan6'}
STATUS_FIELDS = {'schema_version', 'sampled_uptime', 'activation_supported',
                 'ram_bench', 'modules', 'controller', 'supervisor', 'optical',
                 'calibration', 'firmware', 'registration', 'los', 'omci'}


class CollectionError(Exception):
    pass


class Remote:
    def __init__(self, host, known_hosts=None):
        host = str(ipaddress.ip_address(host))
        self.argv = ['ssh', '-T', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
                     '-o', 'StrictHostKeyChecking=yes', '-o', 'ServerAliveInterval=5',
                     '-o', 'ServerAliveCountMax=2']
        if known_hosts:
            self.argv += ['-o', 'UserKnownHostsFile=' + str(known_hosts.resolve())]
        self.argv += ['root@' + host, 'sh', '-s']

    def run(self, script, timeout=30):
        result = subprocess.run(self.argv, input=script, capture_output=True,
                                text=True, timeout=timeout)
        return dict(returncode=result.returncode, stdout=result.stdout, stderr=result.stderr)


def parse_snapshot(result):
    if result['returncode']:
        raise CollectionError('SSH snapshot failed: ' + result['stderr'].strip())
    records = {}
    for line in result['stdout'].splitlines():
        try:
            item = json.loads(line)
            name, rc, raw = item['name'], item['returncode'], item['data']
            if name not in FIELDS or name in records or type(rc) is not int or not isinstance(raw, str):
                raise ValueError()
        except (ValueError, KeyError, TypeError):
            raise CollectionError('Malformed snapshot envelope') from None
        row = records[name] = dict(returncode=rc)
        if rc:
            # A missing interface is useful evidence; never store arbitrary
            # command-error output that might contain configuration values.
            row['error'] = 'unavailable'
            continue
        if name in TEXT_FIELDS:
            row['data'] = raw.strip()
            continue
        try:
            data = json.loads(raw)
        except ValueError:
            row['error'] = 'invalid JSON'
            continue
        if name == 'status':
            if not isinstance(data, dict) or type(data.get('schema_version')) is not int or data['schema_version'] != 1:
                raise CollectionError('Unsupported optical status schema')
            # Identity and factory identifiers from the LuCI status response
            # are unnecessary for this collection; discard before writing.
            data = {k: v for k, v in data.items() if k in STATUS_FIELDS}
        elif name == 'supervisor':
            try:
                instances = data.get('q1000k-xgspon', {}).get('instances', {})
                data = {name: {key: entry.get(key) for key in ('running', 'pid', 'exit_code')}
                        for name, entry in instances.items()}
            except (AttributeError, TypeError):
                raise CollectionError('Malformed supervisor status') from None
        row['data'] = data
    if records.keys() != FIELDS:
        raise CollectionError('Incomplete snapshot')
    if records['board'].get('data') != 'quantum,q1000k-ubi':
        raise CollectionError('Unexpected board')
    if not re.fullmatch(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', records['boot_id'].get('data', '')):
        raise CollectionError('Missing or invalid boot identity')
    status = records['status'].get('data')
    if not isinstance(status, dict) or not all(isinstance(status.get(k), dict) for k in ('modules', 'controller', 'supervisor')):
        raise CollectionError('Optical status unavailable or incomplete')
    return dict(host_time=time.time(), records=records)


def stack_state(snapshot):
    status = snapshot['records']['status']['data']
    phy = status['modules'].get('phy_loaded') is True
    mac = status['modules'].get('mac_loaded') is True
    controller = status['controller'].get('available') is True
    if controller and phy and mac:
        mode = 'full-stack'
    elif controller and not phy and not mac:
        mode = 'controller-only'
    elif controller or phy or mac:
        mode = 'partial-stack'
    else:
        mode = 'unloaded'
    return dict(mode=mode, registration=status.get('registration'), los=status.get('los'),
                supervisor_stage=status['supervisor'].get('last_stage'))


def generation(snapshot):
    rows = snapshot['records']
    status = rows['status']['data']
    return dict(boot_id=rows['boot_id']['data'], modules=status['modules'],
                controller_available=status['controller'].get('available'),
                processes=rows['supervisor'].get('data'))


def probe_address(value):
    address = ipaddress.ip_address(value)
    if address.is_multicast or address.is_unspecified or address.is_loopback or address.is_link_local:
        raise ValueError('Probe target must be a routed unicast IP address')
    return address


def probe(remote, address, snapshot):
    record = dict(target=str(address), interface='pon', traffic_sent=False)
    if stack_state(snapshot)['mode'] != 'full-stack':
        return dict(record, status='skipped', reason='Full PON stack is not loaded')
    family = '-4' if address.version == 4 else '-6'
    route = remote.run(shlex.join(['ip', family, '-j', 'route', 'get', str(address), 'oif', 'pon']) + '\n')
    record['route'] = route
    try:
        routes = json.loads(route['stdout']) if route['returncode'] == 0 else []
        usable = isinstance(routes, list) and bool(routes) and all(isinstance(r, dict) and r.get('dev') == 'pon' for r in routes)
    except ValueError:
        usable = False
    if not usable:
        return dict(record, status='skipped', reason='No verified route through pon')
    command = ['/usr/bin/ping', family, '-I', 'pon', '-c', '3', '-W', '2', '-w', '10', str(address)]
    record['ping'] = remote.run(shlex.join(command) + '\n', timeout=15)
    return dict(record, traffic_sent=True, status='passed' if record['ping']['returncode'] == 0 else 'failed')


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.chmod(0o600)
    temporary.replace(path)


def collect(args, remote, sleep=time.sleep):
    output = args.output.resolve()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    (output / 'snapshot-command.sh').write_text(SNAPSHOT_SCRIPT)
    (output / 'collector.py').write_bytes(Path(__file__).read_bytes())
    record = dict(schema_version=1, mode='observe-existing-stack', host=args.host,
                  started=time.time(), status='collecting', samples=[], probes=[],
                  lifecycle_control=False, network_configuration_changed=False,
                  service_functionality_verified=False,
                  note='Collection success does not prove OLT registration, LAN forwarding or Internet service.')
    write_json(output / 'collection.json', record)
    exitcode = 0
    try:
        baseline = None
        for index in range(args.samples):
            if index:
                sleep(args.interval)
            snapshot = parse_snapshot(remote.run(SNAPSHOT_SCRIPT))
            name = f'sample-{index:04d}.json'
            write_json(output / name, snapshot)
            record['samples'].append(name)
            state = stack_state(snapshot)
            record['final_state'] = state
            if baseline is None:
                baseline = generation(snapshot)
                record['initial_state'] = state
                record['generation'] = baseline
            elif generation(snapshot) != baseline:
                record.update(status='lifecycle-changed', reason='Boot, module presence, controller presence or supervisor process changed externally.')
                exitcode = 3
                break
            write_json(output / 'collection.json', record)
        if exitcode == 0:
            for target in args.probe_ip:
                # Confirm this is still the same service instance before each
                # optional probe. Never recover/reconfigure a changed stack.
                current = parse_snapshot(remote.run(SNAPSHOT_SCRIPT))
                name = f'pre-probe-{len(record["probes"]):02d}.json'
                write_json(output / name, current)
                record['samples'].append(name)
                if generation(current) != baseline:
                    record.update(status='lifecycle-changed', reason='Stack changed before a probe.')
                    exitcode = 3
                    break
                record['probes'].append(probe(remote, probe_address(target), current))
                write_json(output / 'collection.json', record)
        if exitcode == 0:
            final = parse_snapshot(remote.run(SNAPSHOT_SCRIPT))
            write_json(output / 'final.json', final)
            record['samples'].append('final.json')
            record['final_state'] = stack_state(final)
            same = generation(final) == baseline
            record['same_lifecycle_at_end'] = same
            record['status'] = 'completed' if same else 'lifecycle-changed'
            exitcode = 0 if same else 3
    except KeyboardInterrupt:
        record['status'] = 'interrupted'
        exitcode = 130
    except (CollectionError, OSError, subprocess.SubprocessError, ValueError) as error:
        record.update(status='failed', error=str(error))
        exitcode = 1
    finally:
        # Only local evidence is finalized. The service, modules, addresses,
        # routes, leases and optical controls are never part of our cleanup.
        record['finished'] = time.time()
        write_json(output / 'collection.json', record)
        (output / 'sha256sums').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n'
            for p in sorted(output.iterdir()) if p.is_file() and p.name != 'sha256sums'))
    return exitcode, record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='192.168.255.1', help='Router IP address')
    parser.add_argument('--known-hosts', type=Path, help='Existing SSH known-hosts file; default is your normal SSH configuration')
    parser.add_argument('--output', type=Path, help='New evidence directory under build-artifacts/')
    parser.add_argument('--samples', type=int, default=6)
    parser.add_argument('--interval', type=int, default=5, help='Seconds between completed samples (5–60)')
    parser.add_argument('--probe-ip', action='append', default=[], help='Optional routed IP to ping through pon; at most four targets')
    parser.add_argument('--dry-run', action='store_true', help='Print the plan without connecting or creating evidence')
    args = parser.parse_args(argv)
    try:
        args.host = str(ipaddress.ip_address(args.host))
        for value in args.probe_ip:
            probe_address(value)
    except ValueError as error:
        parser.error(str(error))
    if not 1 <= args.samples <= 721 or not 5 <= args.interval <= 60 or len(args.probe_ip) > 4:
        parser.error('Use 1–721 samples, an interval of 5–60 seconds and at most four probe targets')
    if args.known_hosts and not args.known_hosts.is_file():
        parser.error('--known-hosts must name an existing file')
    if args.dry_run:
        print(json.dumps(dict(mode='observe-existing-stack', host=args.host, samples=args.samples,
                             interval=args.interval, probe_ip=args.probe_ip,
                             lifecycle_control=False, network_configuration_changed=False), indent=2))
        return 0
    if args.output is None:
        parser.error('--output is required')
    os.umask(0o077)
    code, record = collect(args, Remote(args.host, args.known_hosts))
    print(json.dumps(record, indent=2))
    return code


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    raise SystemExit(main())
