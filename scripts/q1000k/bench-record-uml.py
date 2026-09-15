#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Attach verified local PHY/core UML evidence to a completed bench artifact."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]


def save(path, data):
    if path.exists():
        if path.read_bytes() != data:
            raise ValueError('Existing evidence differs: ' + str(path))
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact', type=Path, required=True)
    parser.add_argument('--phy-run', type=Path, required=True)
    scope = parser.add_mutually_exclusive_group(required=True)
    scope.add_argument('--core-run', type=Path)
    scope.add_argument('--phy-only', action='store_true',
                       help='Record only the PHY run; do not claim a new OMCI core test')
    args = parser.parse_args()
    artifact, phy = (p.resolve(strict=True) for p in (args.artifact, args.phy_run))
    core = args.core_run.resolve(strict=True) if args.core_run else None
    revision = json.loads((artifact / 'selection.json').read_text())['revision']
    checkpoint = json.loads((artifact / 'checkpoint.json').read_text())
    if checkpoint['status'] != 'passed' or checkpoint['revision'] != revision:
        raise ValueError('A completed, matching build checkpoint is required')
    paths = ['package/kernel/q1000k-omci/src',
             'package/kernel/airoha-pon/src/bsp/include/q1000k_phy_api.h',
             'package/kernel/airoha-pon/src/xpon-en757x/xpon_phy_10g',
             'tests/q1000k/pon_phy_kernel_test.py', 'tests/q1000k/pon_phy_kernel_fixture.c',
             'tests/q1000k/pon_phy_lifecycle_fixture.c', 'tests/q1000k/test_pon_phy_lifecycle.py',
             'tests/q1000k/omci_core_kernel_fixture.c', 'tests/q1000k/omci_telemetry_kernel_fixture.c']
    subprocess.run(['git', 'diff', '--exit-code', revision, '--', *paths], cwd=REPO, check=True)
    generated = subprocess.check_output([sys.executable, REPO / 'tests/q1000k/pon_phy_kernel_test.py'])
    if generated != (phy / 'module/pon_phy_test.c').read_bytes():
        raise ValueError('PHY UML source does not match the checkpoint')
    record = {'revision': revision, 'phy': {'source': str(phy),
              'generated_source_matches': True, 'sha256': hashlib.sha256(generated).hexdigest()}}
    if core:
        record['core'] = {'source': str(core), 'matched_source_files': [],
                         'fixture_injected_files': ['net/xpon/omci/agent.c', 'net/xpon/omci/core.c']}
        prefix = Path('package/kernel/q1000k-omci/src')
        names = subprocess.check_output(['git', 'ls-files', str(prefix)], cwd=REPO, text=True)
        for name in names.splitlines():
            relative = Path(name).relative_to(prefix)
            if str(relative) in record['core']['fixture_injected_files']:
                continue
            if (REPO / name).read_bytes() != (core / 'module' / relative).read_bytes():
                raise ValueError('Core UML source differs: ' + str(relative))
            record['core']['matched_source_files'].append(str(relative))
        # Reconstruct the two deliberate test overlays made by run_omci_core_uml.sh.
        # Plain core runs are accepted; the separate optional CLI overlay is not.
        agent = (REPO / prefix / 'net/xpon/omci/agent.c').read_bytes()
        agent += (REPO / 'tests/q1000k/omci_core_kernel_fixture.c').read_bytes()
        base = (REPO / prefix / 'net/xpon/omci/core.c').read_text()
        base += (REPO / 'tests/q1000k/omci_telemetry_kernel_fixture.c').read_text()
        base = base.replace('static int __init omci_init(void)',
                            'int q1000k_omci_core_test(void);\n\nstatic int __init omci_init(void)')
        needle = '\tret = netlink_register_notifier(&omci_netlink_nb);\n\tif (ret)\n\t\tgoto err_genl;\n'
        if base.count(needle) != 1:
            raise ValueError('Unknown core fixture insertion point')
        base = base.replace(needle, needle + '\n\tret = q1000k_omci_core_test();\n\tif (ret) {\n'
                            '\t\tnetlink_unregister_notifier(&omci_netlink_nb);\n\t\tgoto err_genl;\n\t}\n')
        if agent != (core / 'module/net/xpon/omci/agent.c').read_bytes() or \
                base.encode() != (core / 'module/net/xpon/omci/core.c').read_bytes():
            raise ValueError('Core fixture overlay differs from the tested checkpoint')
    runs = [('uml-phy', phy, 'Q1000K_PON_PHY_TEST_EXIT=0')]
    if core:
        runs.append(('uml-core', core, 'Q1000K_OMCI_CORE_TEST_EXIT=0'))
    for name, source, marker in runs:
        log = (source / 'run.log').read_text()
        if marker not in log or re.search(r'BUG:|WARNING:|Oops:|Kernel panic', log):
            raise ValueError('UML run is not a pass: ' + str(source))
        for filename in ('run.log', 'build.log'):
            save(artifact / name / filename, (source / filename).read_bytes())
        save(artifact / name / 'kernel.config', (source / 'build/.config').read_bytes())
    name = 'uml-source-verification.json' if core else 'uml-phy-source-verification.json'
    save(artifact / name, (json.dumps(record, indent=2) + '\n').encode())
    print(artifact / name)


if __name__ == '__main__':
    main()
