#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test reversible public IPv4 DHCP handoff while the ONU retains its WAN lease.

The client keeps its MAC. Only IPv4 is qualified by this bounded RAM prototype.
"""
import argparse
import contextlib
import fcntl
import ipaddress
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import time
import uuid

from bench_recovery import HostRecovery, SerialRecovery, Heartbeat, competing_dhcp_clients

TOOL = '/usr/sbin/q1000k-ip-passthrough'
STATE = '/var/run/q1000k-ip-passthrough'

def endpoint(value):
    address, port = value.rsplit(':', 1)
    return str(ipaddress.ip_address(address)), int(port)


def bound_entries(text):
    entries = []
    for line in text.splitlines():
        if ' BND ' not in line:
            continue
        try:
            fields = dict(re.findall(r'(\w+)=([^\s]+)', line))
            src, dst = map(endpoint, fields['orig'].split('->'))
            entries.append(dict(src=src, dst=dst, fields=fields, line=line,
                                ttl_decrement=bool(int(fields['ib1'], 16) & (1 << 24)),
                                output_port=(int(fields['ib2'], 16) >> 5) & 15))
        except (KeyError, ValueError):
            continue
    return entries


def assess(samples, transfers, client_mac, hardware, wan_mac):
    """Require routed IPv4 tuples in both PPE directions, no NAT, and HW_OFFLOAD.

    Never treat a configured flowtable, unrelated bind or OFFLOAD as a pass.
    IPv6 debug 'source MAC' is a register-selector word, not an Ethernet MAC.
    """
    entries = bound_entries(samples)
    proven, matching, errors = [], [], []
    for transfer in transfers:
        local = (str(ipaddress.ip_address(transfer['local_ip'])), int(transfer['local_port']))
        remote = (str(ipaddress.ip_address(transfer['remote_ip'])), int(transfer['remote_port']))
        pair = [e for e in entries if (e['src'], e['dst']) in ((local, remote), (remote, local))]
        matching.extend(e['line'] for e in pair)
        ct = [line for line in samples.splitlines() if '[HW_OFFLOAD]' in line and
              f'src={local[0]} ' in line and f'dst={remote[0]} ' in line and
              f'sport={local[1]} ' in line and f'dport={remote[1]} ' in line]
        if not hardware:
            if pair or ct:
                errors.append('Test flow remained in hardware during the software comparison')
            continue
        up = [e for e in pair if e['src'] == local and e['output_port'] == 2]
        down = [e for e in pair if e['src'] == remote and e['output_port'] in (1, 4)]
        if not up or not down or not ct:
            continue
        valid = True
        for entry in up + down:
            f = entry['fields']
            if not entry['ttl_decrement']:
                errors.append('A routed test entry does not decrement TTL')
                valid = False
            if f.get('new') != f['orig']:
                errors.append('A public IPv4 entry changes its IP tuple or omits translated tuple evidence')
                valid = False
            src_mac, dst_mac = f['eth'].lower().split('->')
            if entry['src'] == local and src_mac != wan_mac.lower():
                errors.append('Upstream source MAC is not the ONU WAN MAC')
                valid = False
            if entry['src'] == remote and dst_mac != client_mac.lower():
                errors.append('Downstream destination MAC is not the designated client')
                valid = False
            if entry['src'] == local:
                channel = (int(f['data'], 16) >> 11) & 31
                if not 0 < int(f['etype'], 16) < 0x4000 or not channel or (int(f['ib2'], 16) & 31) != channel:
                    errors.append('Upstream PON GEM/channel/NBQ metadata is invalid')
                    valid = False
        if valid:
            proven.append(dict(local=local, remote=remote))
    if hardware and not proven:
        errors.append('No test connection had verified hardware bindings in both directions')
    return dict(passed=not errors, hardware=hardware, proven_connections=proven,
                matching_bindings=sorted(set(matching)), errors=sorted(set(errors)),
                limits=['PPE header settings are checked; end-to-end wire TTL/MAC capture is still required',
                        'PPE byte counters are not used because this profile disables flow accounting'])


def cpu_busy(samples):
    cpus = []
    for line in samples.splitlines():
        if line.startswith('cpu '):
            values = list(map(int, line.split()[1:9]))
            cpus.append((sum(values), values[3] + values[4]))
    if len(cpus) < 2 or cpus[-1][0] <= cpus[0][0]:
        return None
    return 100 * (1 - (cpus[-1][1] - cpus[0][1]) / (cpus[-1][0] - cpus[0][0]))


def run_bench(stack):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--router', default='192.168.0.1')
    parser.add_argument('--interface', required=True, help='Directly attached Linux Ethernet interface')
    parser.add_argument('--port', choices=('lan1',), required=True)
    parser.add_argument('--known-hosts', type=Path, required=True, help='Verified SSH host key file')
    parser.add_argument('--ssh-key', type=Path, help='SSH private key (useful when running with sudo)')
    parser.add_argument('--manifest', type=Path, required=True, help='Exact-image and RAM helper manifest')
    parser.add_argument('--output', type=Path, required=True, help='New directory under build-artifacts')
    parser.add_argument('--families', choices=('4',), default='4')
    parser.add_argument('--uncapped', action='store_true', help='Remove sampling rate limits for a separate throughput run')
    parser.add_argument('--management-client', help='Unused static /24 management address; default subnet .2')
    parser.add_argument('--serial-device', type=Path, required=True, help='Serial console with an existing reader')
    parser.add_argument('--serial-log', type=Path, required=True, help='Live output log of that serial reader')
    parser.add_argument('--preflight-only', action='store_true', help='Verify identity, serial, management and rollback without changing the WAN or DHCP service')
    parser.add_argument('--dhcp-only', action='store_true', help='Test public-IP DHCP and HTTPS and restoration before running offload traffic')
    parser.add_argument('--count', type=int, default=8, help='25 MB downloads per phase (1..16)')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('Run with sudo: the temporary NetworkManager connection needs administrator access')
    if not 1 <= args.count <= 16:
        parser.error('--count must be 1..16')
    router = ipaddress.IPv4Address(args.router)
    network = ipaddress.IPv4Network(f'{router}/24', strict=False)
    management = ipaddress.IPv4Address(args.management_client or str(network.network_address + 2))
    if management not in network or management in (router, network.network_address, network.broadcast_address):
        parser.error('Choose an unused address in the router management /24')
    if not re.fullmatch(r'[a-zA-Z0-9_.:-]+', args.interface):
        parser.error('Invalid interface name')
    for program in ('nmcli', 'busctl', 'ip', 'ssh', 'curl'):
        if not shutil.which(program):
            parser.error('Required host program missing: ' + program)
    arping = ['arping']
    if not shutil.which('arping'):
        if not shutil.which('busybox') or 'arping' not in subprocess.check_output(['busybox', '--list']).decode().splitlines():
            parser.error('Required host program missing: arping (iputils or BusyBox)')
        arping = ['busybox', 'arping']
    output = args.output.resolve()
    if 'build-artifacts' not in output.parts:
        parser.error('Store raw bench evidence under build-artifacts')
    output.mkdir(mode=0o700)
    lock = stack.enter_context(open('/run/lock/q1000k-ip-handoff-' + args.interface, 'a'))
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    (output / 'collector-input.py').write_bytes(Path(__file__).read_bytes())
    manifest = json.loads(args.manifest.read_text())
    (output / 'image-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    expected = manifest['kernel_notes_sha256']
    import hashlib
    if hashlib.sha256(Path(__file__).read_bytes()).hexdigest() != manifest['collector_sha256']:
        raise ValueError('Collector differs from the reviewed manifest')
    if hashlib.sha256(Path(__file__).with_name('bench_recovery.py').read_bytes()).hexdigest() != manifest['recovery_sha256']:
        raise ValueError('Recovery module differs from the reviewed manifest')
    if not re.fullmatch('[0-9a-f]{64}', expected):
        raise ValueError('Invalid kernel identity in manifest')
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
           '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=2',
           '-o', 'StrictHostKeyChecking=yes', '-o', 'UserKnownHostsFile=' + str(args.known_hosts.resolve()),
           'root@' + str(router)]
    if args.ssh_key:
        ssh[1:1] = ['-i', str(args.ssh_key.resolve()), '-o', 'IdentitiesOnly=yes']

    def local(command, name, timeout=30, check=True):
        try:
            result = subprocess.run(command, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            (output / (name + '.stdout')).write_bytes(exc.stdout or b'')
            (output / (name + '.stderr')).write_bytes(exc.stderr or b'')
            raise
        (output / (name + '.stdout')).write_bytes(result.stdout)
        (output / (name + '.stderr')).write_bytes(result.stderr)
        if check and result.returncode:
            raise RuntimeError(f'{name} failed ({result.returncode}); see captured output')
        return result

    identity = ('set -eu\ntest "$(sha256sum /sys/kernel/notes | cut -d " " -f 1)" = ' + shlex.quote(expected) + '\n')
    for path, sha in manifest['runtime_sha256'].items():
        if not path.startswith('/') or not re.fullmatch('[0-9a-f]{64}', sha):
            raise ValueError('Invalid runtime identity')
        identity += 'test "$(sha256sum ' + shlex.quote(path) + ' | cut -d " " -f 1)" = ' + shlex.quote(sha) + '\n'

    def remote(command, name, timeout=45, check=True):
        return local(ssh + [identity + command], name, timeout, check)

    def signal_stop(signum, frame):
        raise KeyboardInterrupt(f'Interrupted by signal {signum}')

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, signal_stop)
    conflicts = competing_dhcp_clients(args.interface)
    (output / 'competing-dhcp-clients.json').write_text(json.dumps(conflicts, indent=2) + '\n')
    if conflicts:
        raise RuntimeError('External DHCP clients are using this interface; resolve them before testing (see competing-dhcp-clients.json)')
    original = local(['nmcli', '-g', 'GENERAL.CON-UUID', 'device', 'show', args.interface], 'client-original').stdout.decode().strip()
    uuid.UUID(original)  # Refuse a disconnected or unmanaged client interface.
    mac = (Path('/sys/class/net') / args.interface / 'address').read_text().strip()
    original_mac = mac
    if not re.fullmatch(r'([0-9a-f]{2}:){5}[0-9a-f]{2}', mac):
        raise ValueError('Invalid local Ethernet MAC')
    remote(TOOL + ' snapshot', 'router-before')
    boot = remote('cat /proc/sys/kernel/random/boot_id', 'router-boot').stdout.decode().strip()
    uuid.UUID(boot)
    if boot != manifest['expected_boot_id']:
        raise RuntimeError('The ONU rebooted; prepare a fresh identity-verified session')
    identity += 'test "$(cat /proc/sys/kernel/random/boot_id)" = ' + shlex.quote(boot) + '\n'
    remote('test ! -e ' + STATE + '\ntest ! -e /sys/class/net/br-lan/brif/pon\n', 'router-idle')
    remote('tar -C /etc/config -cf - network dhcp firewall', 'router-config-backup')
    (output / 'router-config-backup.stdout').rename(output / 'router-config-backup.tar')
    config_hashes = remote('sha256sum /etc/config/network /etc/config/dhcp /etc/config/firewall',
                           'router-config-hashes').stdout.decode()
    config_check = ''
    for line in config_hashes.splitlines():
        digest, path = line.split()
        if not re.fullmatch('[0-9a-f]{64}', digest) or path not in ('/etc/config/network', '/etc/config/dhcp', '/etc/config/firewall'):
            raise ValueError('Invalid router configuration hash')
        config_check += 'test "$(sha256sum ' + path + ' | cut -d " " -f 1)" = ' + digest + '\n'
    if len(config_hashes.splitlines()) != 3:
        raise ValueError('Router configuration hash capture incomplete')
    pon_original_mac = remote('cat /sys/class/net/pon/address', 'pon-original-mac').stdout.decode().strip()
    wan_before = json.loads(remote('ifstatus wan', 'wan-lease-before').stdout)
    wan_address = wan_before['ipv4-address'][0]['address']
    if mac == pon_original_mac:
        raise ValueError('The client must keep its own MAC, distinct from the ONU WAN MAC')
    config_check += 'test "$(cat /sys/class/net/pon/address)" = ' + shlex.quote(pon_original_mac) + '\n'
    serial = SerialRecovery(args.serial_device, args.serial_log, output, identity)
    token = str(uuid.uuid4())
    cleanup_command = ('test ! -e ' + STATE + '\n'
                       'test ! -e /sys/class/net/br-lan/brif/pon\n') + config_check
    serial.prepare('true', remote, 'serial-preflight')
    serial.prepare(f'{TOOL} stop {token}', remote, 'serial-restore')
    serial.prepare(cleanup_command, remote, 'serial-cleanup-check')
    serial.execute('true', 'serial-preflight', timeout=15)
    connection = 'q1000k-ip-handoff-' + token
    host = HostRecovery(local, args.interface, original, router, management, token, arping)
    heartbeat = Heartbeat(host, remote, token, TOOL)
    entered, created, completed = False, False, False
    router_restored = False
    results, cleanup_errors = [], []
    try:
        host.prepare()
        ssh[1:1] = ['-b', str(management)]
        remote('true', 'independent-management-check')
        if args.preflight_only:
            completed = True
            return
        # Keep the attached computer's observed MAC unchanged.
        local(['nmcli', 'connection', 'add', 'save', 'no', 'type', 'ethernet',
               'ifname', args.interface, 'con-name', connection, 'autoconnect', 'no',
               '802-3-ethernet.cloned-mac-address', mac, 'ipv4.method', 'auto',
               'ipv4.dhcp-timeout', '45', 'ipv4.may-fail', 'no',
               'ipv6.method', 'auto', 'ipv6.may-fail', 'yes'], 'client-create')
        created = True
        entered = True  # Also attempt recovery if SSH is lost during start.
        management_mac = (Path('/sys/class/net') / host.link / 'address').read_text().strip()
        remote(f'{TOOL} start {mac} {management} {management_mac} 1800 {token}', 'passthrough-start', timeout=60)
        heartbeat.start()
        local(['nmcli', '--wait', '90', 'connection', 'up', connection, 'ifname', args.interface], 'client-dhcp', timeout=100)
        handoff_started = time.monotonic()
        heartbeat.check()
        if (Path('/sys/class/net') / args.interface / 'address').read_text().strip() != mac:
            raise RuntimeError('The client MAC changed unexpectedly')
        addresses = json.loads(local(['ip', '-j', 'address', 'show', 'dev', args.interface], 'client-addresses').stdout)
        if not any(a.get('local') == wan_address for d in addresses for a in d.get('addr_info', [])):
            raise RuntimeError('The client did not receive the ONU public WAN address')
        lease = json.loads(remote('ifstatus wan', 'wan-lease-during').stdout)
        if lease['ipv4-address'] != wan_before['ipv4-address']:
            raise RuntimeError('The ONU WAN lease changed during handoff')
        remote('test "$(cat /sys/class/net/pon/address)" = ' + pon_original_mac, 'unchanged-wan-mac')
        for suffix, url in [('ip', 'https://1.1.1.1/cdn-cgi/trace'), ('dns', 'https://speed.cloudflare.com/__down?bytes=1000')]:
            result = local(['curl', '--interface', args.interface, '--noproxy', '*', '-4',
                            '--fail', '--silent', '--show-error', '--connect-timeout', '5', '--max-time', '10',
                            '--output', '/dev/null', '--write-out', '%{json}', url], 'public-https-' + suffix, timeout=15)
            record = json.loads(result.stdout)
            if record['local_ip'] != wan_address or record['http_code'] != 200:
                raise RuntimeError('HTTPS did not use the handed-off public IPv4 address')
        local(['ip', '-j', 'route', 'show', 'table', 'all'], 'client-routes')
        remote(TOOL + ' snapshot', 'passthrough-before')
        if args.dhcp_only:
            completed = True
            return
        for mode in ('hardware', 'software', 'hardware'):
            heartbeat.check()
            phase = f'{len(results):02d}-{mode}'
            remote(f'{TOOL} mode {mode}', phase + '-mode')
            # Give asynchronously destroyed conntrack/PPE entries time to drain.
            time.sleep(2)
            for family in ((4, 6) if args.families == 'both' else (int(args.families),)):
                for direction in ('download', 'upload'):
                    heartbeat.check()
                    name = f'{phase}-v{family}-{direction}'
                    sampler_text = identity + TOOL + ' sample\n'
                    (output / (name + '-sampler.sh')).write_text(sampler_text)
                    samples_path = output / (name + '-samples.txt')
                    with samples_path.open('wb') as stream:
                        sampler = subprocess.Popen(ssh + [sampler_text], stdout=stream, stderr=subprocess.STDOUT)
                        try:
                            time.sleep(1)
                            if sampler.poll() is not None:
                                raise RuntimeError('Router sampler failed before traffic')
                            command = ['curl', '--interface', args.interface, f'-{family}',
                                       '--noproxy', '*', '--http1.1', '--silent', '--show-error',
                                       '--fail', '--fail-early', '--connect-timeout', '5', '--max-time', '15',
                                       '--write-out', '%{json}\n']
                            if not args.uncapped:
                                command += ['--limit-rate', '32M' if direction == 'download' else '16M']
                            payload = None
                            if direction == 'upload':
                                payload = output / 'generated-upload.bin'
                                if not payload.exists():
                                    with payload.open('wb') as data:
                                        data.truncate(16 * 1024 * 1024)
                                command += ['--request', 'POST', '--data-binary', '@' + str(payload)]
                            url = ('https://speed.cloudflare.com/__down?bytes=25000000' if direction == 'download'
                                   else 'https://speed.cloudflare.com/__up')
                            for _ in range(args.count if direction == 'download' else 4):
                                command += ['--output', '/dev/null', '--url', url]
                            started = time.monotonic()
                            transfer = local(command, name + '-curl', timeout=150, check=False)
                            elapsed = time.monotonic() - started
                            time.sleep(1)
                        finally:
                            try:
                                remote(TOOL + ' stop-sample', name + '-stop-sample')
                            finally:
                                sampler.terminate()
                                try:
                                    sampler.wait(timeout=5)
                                except subprocess.TimeoutExpired:
                                    sampler.kill()
                                    sampler.wait(timeout=5)
                    transfers = [json.loads(line) for line in transfer.stdout.splitlines()]
                    expected_count = args.count if direction == 'download' else 4
                    if transfer.returncode or len(transfers) != expected_count:
                        raise RuntimeError(f'{name}: incomplete transfers, no offload verdict')
                    for part in transfers:
                        address = ipaddress.ip_address(part['local_ip'])
                        if address.version != 4 or str(address) != wan_address:
                            raise RuntimeError('Test still uses the ONU management subnet, not the ISP lease')
                        if int(part['http_code']) != 200:
                            raise RuntimeError('HTTP test endpoint did not return success')
                        if direction == 'download' and int(part['size_download']) != 25000000:
                            raise RuntimeError('Download size mismatch')
                        if direction == 'upload' and int(part['size_upload']) != 16 * 1024 * 1024:
                            raise RuntimeError('Upload size mismatch')
                    samples = samples_path.read_text(errors='replace')
                    report = assess(samples, transfers, mac, mode == 'hardware', pon_original_mac)
                    report.update(name=name, family=family, direction=direction,
                                  Mbps=sum(p['size_download' if direction == 'download' else 'size_upload'] for p in transfers) * 8 / elapsed / 1e6,
                                  cpu_busy_percent=cpu_busy(samples),
                                  rate_limit=None if args.uncapped else ('32M' if direction == 'download' else '16M'))
                    results.append(report)
                    (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
                    print(json.dumps({k: report[k] for k in ('name', 'passed', 'Mbps', 'cpu_busy_percent', 'errors')}), flush=True)
            remote(TOOL + ' snapshot', phase + '-after')
        # The downstream lease lasts 120s; retain the same session beyond T1
        # so LAN packet capture can prove a unicast renewal without a new handoff.
        while time.monotonic() - handoff_started < 75:
            heartbeat.check()
            time.sleep(1)
        local(['nmcli', '-g', 'DHCP4.OPTION', 'device', 'show', args.interface], 'post-renewal-dhcp-options')
        lease = json.loads(remote('ifstatus wan', 'post-renewal-wan-lease').stdout)
        if lease['ipv4-address'] != wan_before['ipv4-address']:
            raise RuntimeError('ONU WAN lease changed during the renewal observation')
        check = local(['curl', '--interface', args.interface, '--noproxy', '*', '-4', '--fail', '--silent',
                       '--show-error', '--connect-timeout', '5', '--max-time', '10', '--output', '/dev/null',
                       '--write-out', '%{json}', 'https://1.1.1.1/cdn-cgi/trace'], 'post-renewal-https', timeout=15)
        if json.loads(check.stdout)['local_ip'] != wan_address:
            raise RuntimeError('The client lost its public IPv4 lease')
        completed = True
    finally:
        # Do not allow a second Ctrl-C to interrupt recovery. SIGKILL still
        # leaves the ONU guard and NetworkManager checkpoint running.
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(sig, signal.SIG_IGN)
        try:
            heartbeat.stop()
        except Exception as exc:
            cleanup_errors.append(str(exc))
        if entered:
            try:
                remote(TOOL + ' snapshot', 'passthrough-final', timeout=15)
            except Exception as exc:
                (output / 'final-snapshot-error.txt').write_text(str(exc) + '\n')
            try:
                remote(f'{TOOL} stop {token}', 'restore-router', timeout=45)
            except Exception as exc:
                (output / 'ssh-restore-error.txt').write_text(str(exc) + '\n')
                try:
                    serial.execute(f'{TOOL} stop {token}', 'serial-restore', timeout=60)
                except Exception as fallback:
                    cleanup_errors.append(str(fallback))
        try:
            command = cleanup_command
            try:
                remote(command, 'cleanup-check', timeout=15)
            except Exception as exc:
                serial.execute(command, 'serial-cleanup-check', timeout=20)
            router_restored = True
            if entered:
                try:
                    archive = remote('if [ -f /var/run/q1000k-ip-passthrough-last ]; then\n'
                                     'd=$(cat /var/run/q1000k-ip-passthrough-last)\n'
                                     'if [ "$(cat "$d/token")" = ' + token + ' ]; then tar -C "$d" -cf - .; fi\nfi\n', 'router-transaction', timeout=30)
                    if archive.stdout:
                        (output / 'router-transaction.stdout').rename(output / 'router-transaction.tar')
                except Exception as error:
                    cleanup_errors.append('Evidence archive: ' + str(error))
            # Archive failure must not prevent rollback of an already verified
            # router and host, nor leave an unnecessary management /32 route.
            host.restore()
            # An active original NM profile can still hold the temporary public
            # lease. Keep independent management until the private route works.
            def private_route_ready():
                result = local(['ip', '-j', '-4', 'route', 'get', '1.1.1.1', 'oif', args.interface],
                               'restored-private-route', check=False)
                routes = json.loads(result.stdout) if result.returncode == 0 else []
                return any(r.get('gateway') == str(router) and
                           ipaddress.ip_address(r.get('prefsrc', r.get('src', '0.0.0.0'))) in network
                           for r in routes)
            if not private_route_ready():
                local(['nmcli', '--wait', '90', 'connection', 'up', 'uuid', original,
                       'ifname', args.interface], 'restore-private-lease', timeout=100)
                for _ in range(20):
                    if private_route_ready():
                        break
                    time.sleep(1)
                else:
                    raise RuntimeError('Host private DHCP route did not return; retaining fixed management')
            if (Path('/sys/class/net') / args.interface / 'address').read_text().strip() != original_mac:
                raise RuntimeError('Original physical client MAC was not restored')
            if created:
                local(['nmcli', 'connection', 'delete', connection], 'delete-temporary-client')
            host.remove_management()
            ssh.remove(str(management))
            ssh.remove('-b')
            ssh[1:1] = ['-B', args.interface]
            if entered:
                remote('if [ -f ' + STATE + '-last ]; then d=$(cat ' + STATE + '-last);\n'
                       'if [ "$(cat "$d/token")" = ' + token + ' ]; then ' + TOOL + ' management-release ' + token + '; fi; fi',
                       'release-management-pin')
        except Exception as exc:
            cleanup_errors.append(str(exc))
        try:
            local(['journalctl', '-u', 'NetworkManager', '--since', '-20min', '--no-pager'],
                  'networkmanager-journal', check=False)
        except Exception as exc:
            cleanup_errors.append('Journal capture: ' + str(exc))
        (output / 'summary.json').write_text(json.dumps(dict(
            passed=completed and (args.preflight_only or args.dhcp_only or bool(results)) and all(r['passed'] for r in results) and not cleanup_errors,
            completed=completed,
            results=results, cleanup_errors=cleanup_errors,
            preflight_only=args.preflight_only, dhcp_only=args.dhcp_only, router_restored=router_restored,
            retained_management_link=host.link if host.link_created else None,
            checkpoint=host.checkpoint,
            transaction_token=token,
            firmware=manifest, client_interface=args.interface, client_mac=mac,
            optical_outage_requested=False), indent=2) + '\n')
        if (args.preflight_only or args.dhcp_only) and cleanup_errors:
            raise RuntimeError('Preflight cleanup failed; see summary.json')
    if cleanup_errors or not results or not all(r['passed'] for r in results):
        raise SystemExit('Bench did not pass completely; inspect summary.json and retained evidence')


def main():
    with contextlib.ExitStack() as stack:
        return run_bench(stack)


if __name__ == '__main__':
    main()
