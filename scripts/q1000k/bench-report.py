#!/usr/bin/env python3
"""Summarize saved, completed disconnected-fiber stack captures; no SSH access."""
import argparse
import json
import re
from pathlib import Path


def summarize(capture):
    capture = capture.resolve(strict=True)
    record = json.loads((capture / 'checkpoint.json').read_text())
    for field, expected in {
        'schema_version': 1, 'action': 'stack', 'host': '192.168.255.1',
        'status': 'passed', 'postflight': 'passed', 'input_cleanup': 'passed',
    }.items():
        if record.get(field) != expected:
            raise ValueError(f'{capture}: {field} is not {expected!r}')
    controller, omci, protocol = [], [], []
    for line in (capture / 'attempt.log').read_text().splitlines():
        if line.startswith('protocol_error='):
            protocol.append(int(line.split('=', 1)[1]))
        if not line.startswith('{'):
            continue
        item = json.loads(line)
        if item.get('mode') == 'xgspon':
            controller.append(item)
        elif 'mib_objects' in item:
            omci.append(item)
    if len(omci) != 5 or len(controller) != 6 or protocol != [0] * 5:
        raise ValueError(f'{capture}: incomplete five-sample stack observation')
    for item in controller:
        if any(item.get(key) is not True for key in (
            'gpon_detected', 'xgspon_detected', 'md32_enabled', 'tx_disabled',
            'tx_inhibited', 'calibration_supplied', 'firmware_verified', 'los',
        )) or item.get('last_error') != 0:
            raise ValueError(f'{capture}: controller observation failed')
    for item in omci:
        for key, expected in {
            'schema_version': 1, 'state': 1, 'onu_id': 65535, 'gem_port_id': 65535,
            'agent_enabled': 1, 'agent_operational': 0, 'authenticated': 0,
            'service_rules': 0, 'service_error': 0, 'rx_packets': '0',
            'rx_dropped': '0', 'tx_packets': '0', 'tx_errors': '0',
        }.items():
            if item.get(key) != expected:
                raise ValueError(f'{capture}: unexpected OMCI {key}')
        if type(item.get('mib_objects')) is not int or item['mib_objects'] <= 0:
            raise ValueError(f'{capture}: missing initial MIB')
    serial = (capture / 'serial.log').read_text()
    if re.search(r'BUG:|WARNING:|Oops:|Kernel panic|initialization failed|'
                 r'(?:shutdown|reconfigure|activation) failed|'
                 r'FE write .*expected', serial):
        raise ValueError(f'{capture}: kernel failure diagnostic in serial capture')
    return {
        'capture': str(capture), 'boot_revision': record['revision'],
        'observations': len(omci), 'controller_tx_disabled': True,
        'tx_inhibited': True, 'los': True, 'protocol_error': 0,
        'omci_state': 1, 'mib_objects': sorted({x['mib_objects'] for x in omci}),
        'service_error': 0, 'cleanup': 'passed',
        'serial_start': record['serial_start'],
        'elapsed_seconds': round(record['finished'] - record['started'], 3),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('captures', type=Path, nargs='+')
    args = parser.parse_args()
    try:
        runs = [summarize(path) for path in args.captures]
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'Cannot report a passing bench: {error}\n')
    print(json.dumps({
        'schema_version': 1, 'result': 'disconnected-fiber stack passed',
        'optical_service_verified': False, 'runs': runs,
    }, indent=2))


if __name__ == '__main__':
    main()
