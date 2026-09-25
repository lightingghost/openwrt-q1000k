# SPDX-License-Identifier: GPL-2.0-only
"""Recovery primitives retained from the paced-serial L2 bench workflow."""
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
import socket
import struct
import subprocess
import threading
import time
import uuid

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
        self.run(['sysctl', '-w', 'net.ipv6.conf.' + self.link + '.disable_ipv6=1'], 'management-ipv4-only')
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
        self.prepared = {}

    def prepare(self, command, remote, name):
        """Upload while SSH works; execution later needs only the serial console."""
        nonce = uuid.uuid4().hex
        path = '/tmp/q1000k-serial-' + nonce
        script = self.identity + command + '\n'
        (self.output / (name + '-input.sh')).write_text(script)
        remote("umask 077\ncat > " + path + " <<'" + nonce + "'\n" + script + nonce + '\n',
               name + '-stage')
        self.prepared[command] = path

    def execute(self, command, name, timeout=60):
        marker = 'Q1000K_' + uuid.uuid4().hex
        with self.device.open('wb', buffering=0) as serial:
            serial.write(b'\x03')  # Cancel any partial line left by a UART overrun.
            time.sleep(.5)
            serial.write(b'\r')
            time.sleep(2)  # Wake an idle OpenWrt console before sending a command.
            before = self.log.stat()
            offset = before.st_size
            invocation = ('sh ' + self.prepared[command] if command in self.prepared
                          else 'sh -c ' + shlex.quote(self.identity + command))
            wrapped = invocation + '; rc=$?; printf "\\n%s=%s\\n" ' + marker + ' "$rc"\r'
            encoded = wrapped.encode()
            for byte in encoded:
                serial.write(bytes([byte]))
                time.sleep(.02)
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
    def __init__(self, host, remote, token, tool):
        self.host, self.remote, self.token, self.tool = host, remote, token, tool
        self.done, self.error = threading.Event(), None
        self.thread = threading.Thread(target=self.loop, daemon=True)

    def beat(self, number):
        # Renew host first. If this fails, leave the shorter ONU timer alone.
        self.host.renew(f'heartbeat-{number}-host')
        self.remote(f'{self.tool} heartbeat {self.token}', f'heartbeat-{number}-onu', timeout=15)

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


