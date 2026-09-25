#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Enter a guarded PON bridge test, measure from a LAN client, restore routing.

Run on the directly attached Linux/NetworkManager client. No firmware flashing.
Raw output must be saved under build-artifacts. The router has its own watchdog.
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
import threading
import time
import uuid

TOOL = '/usr/sbin/q1000k-pon-passthrough'
STATE = '/var/run/q1000k-pon-passthrough'


def competing_dhcp_clients(interface, proc=Path('/proc')):
    """An external DHCP client can change addresses/routes behind NM's back."""
    found = []
    for path in proc.glob('[0-9]*/cmdline'):
        try:
            argv = path.read_bytes().decode(errors='replace').strip('\0').split('\0')
        except (FileNotFoundError, ProcessLookupError):
            continue
        if argv and Path(argv[0]).name in ('dhclient', 'dhcpcd', 'udhcpc'):
            # A daemon without an explicit interface may manage all interfaces.
            if interface in argv or not any((Path('/sys/class/net') / arg).is_dir()
                                            for arg in argv[1:] if not arg.startswith('-')):
                found.append(dict(pid=int(path.parent.name), argv=argv))
    return found


class HostRecovery:
    """NM owns the rollback timer; the collector owns only its virtual link."""
    def __init__(self, run, interface, original, router, management, token, arping=('arping',)):
        self.run, self.interface, self.original = run, interface, original
        self.router, self.management = str(router), str(management)
        self.link = 'qmg' + token.replace('-', '')[:8]
        self.checkpoint = None
        self.link_created = False
        self.arping = list(arping)

    def call(self, method, signature, *values, name):
        result = self.run(['busctl', '--system', '--json=short', 'call',
                          'org.freedesktop.NetworkManager', '/org/freedesktop/NetworkManager',
                          'org.freedesktop.NetworkManager', method, signature, *map(str, values)], name)
        return json.loads(result.stdout)['data'][0] if result.stdout.strip() else None

    def prepare(self):
        routes = json.loads(self.run(['ip', '-j', 'route', 'show', 'exact', self.router + '/32'],
                                    'management-route-before').stdout)
        if routes:
            raise RuntimeError('A router /32 route already exists; refusing to replace it')
        addresses = json.loads(self.run(['ip', '-j', 'address', 'show'], 'management-addresses-before').stdout)
        if any(a.get('local') == self.management for d in addresses for a in d.get('addr_info', [])):
            raise RuntimeError('Management client address already exists on the host')
        device = self.call('GetDeviceByIpIface', 's', self.interface, name='checkpoint-device')
        self.checkpoint = self.call('CheckpointCreate', 'aouu', 1, device, 210, 0, name='checkpoint-create')
        if not isinstance(self.checkpoint, str) or not self.checkpoint.startswith('/org/freedesktop/NetworkManager/Checkpoint/'):
            raise RuntimeError('NetworkManager did not return a checkpoint')
        self.run(['ip', 'link', 'add', 'link', self.interface, 'name', self.link,
                  'type', 'macvlan', 'mode', 'bridge'], 'management-create')
        self.link_created = True
        self.run(['nmcli', 'device', 'set', self.link, 'managed', 'no'], 'management-unmanaged')
        self.run(['ip', 'link', 'set', self.link, 'up'], 'management-up')
        # Duplicate-address detection runs before assigning the address.
        self.run(self.arping + ['-D', '-I', self.link, '-c', '3', '-w', '4', self.management], 'management-dad')
        self.run(['ip', 'address', 'add', self.management + '/32', 'dev', self.link], 'management-address')
        self.run(['ip', 'route', 'add', self.router + '/32', 'dev', self.link,
                  'scope', 'link', 'src', self.management], 'management-route')

    def renew(self, name):
        # ONU expires at 90s. Host expires later, after the ONU returns to routing.
        self.call('CheckpointAdjustRollbackTimeout', 'ou', self.checkpoint, 210, name=name)

    def restore(self):
        if self.checkpoint:
            try:
                result = self.call('CheckpointRollback', 'o', self.checkpoint, name='checkpoint-rollback')
                values = result.values() if isinstance(result, dict) else (v for _, v in result)
                if any(value != 0 for value in values):
                    raise RuntimeError('NetworkManager checkpoint rollback was incomplete')
            except Exception:
                # It may already have timed out. Reactivation below is explicit
                # and also retries a transient DHCP failure during rollback.
                self.run(['nmcli', '--wait', '90', 'connection', 'up', 'uuid', self.original,
                          'ifname', self.interface], 'restore-client-retry', timeout=100)
            for _ in range(20):
                current = self.run(['nmcli', '-g', 'GENERAL.CON-UUID,GENERAL.STATE', 'device', 'show',
                                    self.interface], 'restore-client-state').stdout.decode().splitlines()
                if current and current[0] == self.original and any(s.startswith('100 ') for s in current[1:]):
                    break
                time.sleep(1)
            else:
                raise RuntimeError('Original host connection is not active after rollback')
            # Rollback normally destroys its checkpoint. Never destroy another
            # checkpoint or clear all checkpoints with an empty path.
            self.run(['busctl', '--system', 'call', 'org.freedesktop.NetworkManager',
                      '/org/freedesktop/NetworkManager', 'org.freedesktop.NetworkManager',
                      'CheckpointDestroy', 'o', self.checkpoint], 'checkpoint-destroy', check=False)
            self.checkpoint = None

    def remove_management(self):
        if self.link_created:
            self.run(['ip', 'link', 'delete', self.link], 'management-delete')
            self.link_created = False


class SerialRecovery:
    """Use the existing serial reader's log; never compete for serial input."""
    def __init__(self, device, log, output, identity):
        self.device, self.log, self.output, self.identity = device, log, output, identity

    def execute(self, command, name, timeout=60):
        marker = 'Q1000K_' + uuid.uuid4().hex
        with self.device.open('wb', buffering=0) as serial:
            serial.write(b'\r')
            time.sleep(2)  # Wake an idle OpenWrt console before sending a command.
            before = self.log.stat()
            offset = before.st_size
            checked = self.identity + command
            wrapped = 'sh -c ' + shlex.quote(checked) + '; rc=$?; printf "\\n%s=%s\\n" ' + marker + ' "$rc"\r'
            serial.write(wrapped.encode())
            deadline = time.monotonic() + timeout
            observed = b''
            while time.monotonic() < deadline:
                current = self.log.stat()
                if current.st_ino != before.st_ino or current.st_size < offset:
                    raise RuntimeError('Serial log was rotated or truncated during recovery')
                with self.log.open('rb') as stream:
                    stream.seek(offset)
                    observed = stream.read()
                match = re.search(rb'(?:^|\n)' + marker.encode() + rb'=(\d+)\r?(?:\n|$)', observed)
                if match:
                    (self.output / (name + '.serial')).write_bytes(observed)
                    if int(match[1]):
                        raise RuntimeError(name + ': serial identity or command failed')
                    return
                time.sleep(.25)
            (self.output / (name + '.serial')).write_bytes(observed)
            raise RuntimeError(name + ': serial response missing')


class Heartbeat:
    def __init__(self, host, remote, token):
        self.host, self.remote, self.token = host, remote, token
        self.done, self.error = threading.Event(), None
        self.thread = threading.Thread(target=self.loop, daemon=True)

    def beat(self, number):
        # Renew host first. If this fails, leave the shorter ONU timer alone.
        self.host.renew(f'heartbeat-{number}-host')
        self.remote(f'{TOOL} heartbeat {self.token}', f'heartbeat-{number}-onu', timeout=15)

    def start(self):
        self.beat(0)
        self.thread.start()

    def loop(self):
        number = 0
        while not self.done.wait(10):
            number += 1
            try:
                self.beat(number)
            except Exception as exc:
                self.error = exc
                return

    def check(self):
        if self.error:
            raise RuntimeError('Recovery heartbeat failed: ' + str(self.error))

    def stop(self):
        self.done.set()
        if self.thread.is_alive():
            self.thread.join(timeout=50)
            if self.thread.is_alive():
                raise RuntimeError('Heartbeat did not stop; retaining recovery state')


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


def assess(samples, transfers, client_mac, hardware):
    """Require matching live tuples, both PPE directions, no NAT, and HW_OFFLOAD.

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
            if entry['ttl_decrement']:
                errors.append('A bridged test entry decrements TTL/hop limit')
                valid = False
            if 'new' in f and f['new'] != f['orig']:
                errors.append('A bridged IPv4 test entry changes its IP tuple')
                valid = False
            src_mac, dst_mac = f['eth'].lower().split('->')
            if ipaddress.ip_address(local[0]).version == 6:
                source_word = int(src_mac.replace(':', '')[:8], 16)
                if ((source_word >> 16) & 31) != 15:
                    errors.append('IPv6 source-MAC preservation selector is not 0xf')
                    valid = False
            elif entry['src'] == local and src_mac != client_mac.lower():
                errors.append('Upstream IPv4 source MAC is not the attached client')
                valid = False
            if entry['src'] == remote and dst_mac != client_mac.lower():
                errors.append('Downstream destination MAC is not the attached client')
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
    parser.add_argument('--port', choices=('lan1', 'lan2'), required=True)
    parser.add_argument('--known-hosts', type=Path, required=True, help='Verified SSH host key file')
    parser.add_argument('--ssh-key', type=Path, help='SSH private key (useful when running with sudo)')
    parser.add_argument('--manifest', type=Path, required=True, help='Exact-image bridge-bench-manifest.json')
    parser.add_argument('--output', type=Path, required=True, help='New directory under build-artifacts')
    parser.add_argument('--families', choices=('4', '6', 'both'), default='4')
    parser.add_argument('--uncapped', action='store_true', help='Remove sampling rate limits for a separate throughput run')
    parser.add_argument('--management-client', help='Unused static /24 management address; default subnet .2')
    parser.add_argument('--serial-device', type=Path, required=True, help='Serial console with an existing reader')
    parser.add_argument('--serial-log', type=Path, required=True, help='Live output log of that serial reader')
    parser.add_argument('--preflight-only', action='store_true', help='Verify identity, serial, management and rollback without bridging PON')
    parser.add_argument('--dhcp-only', action='store_true', help='Test the ISP lease and restoration before running offload traffic')
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
    lock = stack.enter_context(open('/run/lock/q1000k-bridge-' + args.interface, 'a'))
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    (output / 'collector-input.py').write_bytes(Path(__file__).read_bytes())
    manifest = json.loads(args.manifest.read_text())
    (output / 'image-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    expected = manifest['kernel_notes_sha256']
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
    if not re.fullmatch(r'([0-9a-f]{2}:){5}[0-9a-f]{2}', mac):
        raise ValueError('Invalid local Ethernet MAC')
    remote(TOOL + ' snapshot', 'router-before')
    boot = remote('cat /proc/sys/kernel/random/boot_id', 'router-boot').stdout.decode().strip()
    uuid.UUID(boot)
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
    serial = SerialRecovery(args.serial_device, args.serial_log, output, identity)
    serial.execute('true', 'serial-preflight', timeout=15)
    token = str(uuid.uuid4())
    connection = 'q1000k-bridge-' + token
    host = HostRecovery(local, args.interface, original, router, management, token, arping)
    heartbeat = Heartbeat(host, remote, token)
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
        # Preserve the observed MAC, including an explicitly configured clone.
        local(['nmcli', 'connection', 'add', 'save', 'no', 'type', 'ethernet',
               'ifname', args.interface, 'con-name', connection, 'autoconnect', 'no',
               '802-3-ethernet.cloned-mac-address', mac, 'ipv4.method', 'auto',
               'ipv4.dhcp-timeout', '45', 'ipv4.may-fail', 'no',
               'ipv6.method', 'auto', 'ipv6.may-fail', 'yes'], 'client-create')
        created = True
        entered = True  # Also attempt recovery if SSH is lost during start.
        management_mac = (Path('/sys/class/net') / host.link / 'address').read_text().strip()
        remote(f'{TOOL} start {args.port} {mac} 1800 {token} {management} {management_mac}', 'bridge-start', timeout=60)
        heartbeat.start()
        local(['nmcli', '--wait', '90', 'connection', 'up', connection, 'ifname', args.interface], 'client-dhcp', timeout=100)
        local(['ip', '-j', 'address', 'show', 'dev', args.interface], 'client-addresses')
        local(['ip', '-j', 'route', 'show', 'table', 'all'], 'client-routes')
        remote(TOOL + ' snapshot', 'bridge-before')
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
                        if address.version == 4 and address in network:
                            raise RuntimeError('Test still uses the ONU management subnet, not the ISP lease')
                        if int(part['http_code']) != 200:
                            raise RuntimeError('HTTP test endpoint did not return success')
                        if direction == 'download' and int(part['size_download']) != 25000000:
                            raise RuntimeError('Download size mismatch')
                        if direction == 'upload' and int(part['size_upload']) != 16 * 1024 * 1024:
                            raise RuntimeError('Upload size mismatch')
                    samples = samples_path.read_text(errors='replace')
                    report = assess(samples, transfers, mac, mode == 'hardware')
                    report.update(name=name, family=family, direction=direction,
                                  Mbps=sum(p['size_download' if direction == 'download' else 'size_upload'] for p in transfers) * 8 / elapsed / 1e6,
                                  cpu_busy_percent=cpu_busy(samples),
                                  rate_limit=None if args.uncapped else ('32M' if direction == 'download' else '16M'))
                    results.append(report)
                    (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
                    print(json.dumps({k: report[k] for k in ('name', 'passed', 'Mbps', 'cpu_busy_percent', 'errors')}), flush=True)
            remote(TOOL + ' snapshot', phase + '-after')
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
                remote(TOOL + ' snapshot', 'bridge-final', timeout=15)
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
            command = ('test ! -e ' + STATE + '/active\n'
                       'test ! -e /sys/class/net/br-lan/brif/pon\n')
            command += config_check
            try:
                remote(command, 'cleanup-check', timeout=15)
            except Exception as exc:
                serial.execute(command, 'serial-cleanup-check', timeout=20)
            router_restored = True
            host.restore()
            if created:
                local(['nmcli', 'connection', 'delete', connection], 'delete-temporary-client')
            if entered:
                archive = remote('if [ -f /var/run/q1000k-pon-passthrough-last ]; then\n'
                                 'd=$(cat /var/run/q1000k-pon-passthrough-last)\n'
                                 'test -d "$d" && tar -C "$d" -cf - .\nfi\n', 'router-transaction', timeout=30)
                if archive.stdout:
                    (output / 'router-transaction.stdout').rename(output / 'router-transaction.tar')
            host.remove_management()
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
