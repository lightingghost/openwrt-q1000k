#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Create a private, opt-in continuous-service RAM overlay. Never access hardware."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shlex
import tarfile

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('activation_inputs', Path(__file__).with_name('activation-collect.py'))
inputs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inputs)


def generate(archive, identity_path, destination):
    """Return file hashes/modes, never raw identity, after strict input checks."""
    supplied = json.loads(identity_path.read_text())
    identity = inputs.validate_identity(supplied)
    validated = inputs.private_inputs(archive, identity)
    files = {}
    with tarfile.open(fileobj=io.BytesIO(validated), mode='r:') as source:
        for name in inputs.INPUTS:
            files['lib/firmware/airoha/q1000k/' + name] = (source.extractfile(name).read(), 0o600)
    defaults = dict(sync_circuit_pack='1', mib_profile='native-pptp', fix_vlans='0', olt_profile='auto')
    lines = ["config identity 'identity'"]
    for key in inputs.KEYS:
        value = identity.get(key, defaults.get(key, ''))
        lines.append('\toption ' + key + ' ' + shlex.quote(value))
    lines += ['', "config service 'service'", "\toption monitor '1'", "\toption enabled '1'",
              "\toption lower 'ponraw'", "\toption continuous_bench '1'", '',
              "config passthrough 'passthrough'", "\toption mode 'router'", "\toption client_mac ''", '']
    files['etc/config/xgspon'] = ('\n'.join(lines).encode(), 0o600)
    files['etc/q1000k-private-autostart'] = (b'continuous-activation-v1\n', 0o600)
    files['etc/uci-defaults/zz-q1000k-private-autostart'] = (
        (REPO / 'package/network/utils/q1000k-xgspon-service/files/continuous-defaults').read_bytes(), 0o755)
    destination.mkdir(mode=0o700)
    manifest = {}
    for name, (data, mode) in files.items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(mode)
        manifest[name] = dict(sha256=hashlib.sha256(data).hexdigest(), mode=mode)
    return dict(schema_version=1, profile='continuous-activation-v1', files=manifest,
                inputs_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                identity_sha256=hashlib.sha256(identity_path.read_bytes()).hexdigest(),
                registration_source='explicit' if supplied.get('registration_id') else 'zero-default')
