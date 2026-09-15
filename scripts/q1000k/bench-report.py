#!/usr/bin/env python3
"""Verify saved stack/receive bench observations and cleanup; no SSH access."""
import argparse
import json
import re
from pathlib import Path


def receive_summary(samples, fiber, reacquire=False):
    last_sample, last_poll, last_frames, stable = -1, 0, None, 0
    last_attempts = 0
    for item in samples:
        for key, expected in {
            'rx_bench': True, 'tx_inhibited': True,
            'registration_enabled': False, 'tx_enabled': False,
        }.items():
            if item.get(key) is not expected:
                raise ValueError('Receive guard failed: ' + key)
        for key in ('mac_irq_mask', 'sync_status', 'frames', 'lof', 'fec_total',
                    'fec_corrected', 'fec_uncorrected', 'irq_calls', 'poll_calls', 'sampled_ms'):
            if type(item.get(key)) is not int or item[key] < 0:
                raise ValueError('Invalid receive counter: ' + key)
        for key in ('controller_los', 'phy_los', 'synced'):
            if type(item.get(key)) is not bool:
                raise ValueError('Invalid receive flag: ' + key)
        if (item['mac_irq_mask'] or item['sampled_ms'] <= last_sample or
                item['poll_calls'] < last_poll):
            raise ValueError('Receive IRQ mask or sample freshness failed')
        if reacquire or 'reacquire_enabled' in item or 'reacquire_attempts' in item:
            attempts = item.get('reacquire_attempts')
            if (item.get('reacquire_enabled') is not reacquire or
                    type(attempts) is not int or not last_attempts <= attempts <= int(reacquire) or
                    (attempts and item['poll_calls'] < 10)):
                raise ValueError('Receive reacquisition guard failed')
            last_attempts = attempts
        lit = not item['controller_los'] and not item['phy_los'] and item['synced']
        if fiber == 'disconnected':
            if not item['controller_los'] or not item['phy_los'] or item['synced']:
                raise ValueError('Disconnected receive has unexpected light/sync')
        stable = stable + 1 if lit and last_frames is not None and item['frames'] != last_frames else 0
        last_sample, last_poll, last_frames = item['sampled_ms'], item['poll_calls'], item['frames']
    if not last_poll or (fiber == 'connected' and stable < 5):
        raise ValueError('Receive polling or final downstream stability failed')
    return {
        'samples': len(samples), 'registration_enabled': False, 'mac_irq_mask': 0,
        'final_stable_intervals': stable,
        'counters': {key: {'first': samples[0][key], 'last': samples[-1][key]}
                     for key in ('sampled_ms', 'frames', 'lof', 'fec_total', 'fec_corrected',
                                 'fec_uncorrected', 'irq_calls', 'poll_calls')},
        'controller_los': sorted({x['controller_los'] for x in samples}),
        'phy_los': sorted({x['phy_los'] for x in samples}),
        'synced': sorted({x['synced'] for x in samples}),
        'reacquire_requested': reacquire, 'reacquire_attempts': last_attempts,
    }


def summarize(capture):
    capture = capture.resolve(strict=True)
    record = json.loads((capture / 'checkpoint.json').read_text())
    for field, expected in {
        'schema_version': 1, 'host': '192.168.255.1',
        'status': 'passed', 'postflight': 'passed', 'input_cleanup': 'passed',
    }.items():
        if record.get(field) != expected:
            raise ValueError(f'{capture}: {field} is not {expected!r}')
    action, fiber = record.get('action'), record.get('fiber')
    reacquire = record.get('reacquire_once', False)
    if type(reacquire) is not bool or (reacquire and (action != 'receive' or fiber != 'connected')):
        raise ValueError('Invalid receive reacquisition request')
    if action not in ('stack', 'receive') or fiber not in ('connected', 'disconnected'):
        raise ValueError('Unknown bench action/fiber state')
    if action == 'stack' and fiber != 'disconnected':
        raise ValueError('Normal stack test requires disconnected fiber')
    count = 30 if action == 'receive' else 5
    controller, omci, protocol, receive = [], [], [], []
    leds = {'green:wan-1': [], 'red:wan': []}
    for line in (capture / 'attempt.log').read_text().splitlines():
        led = re.fullmatch(r'fiber_led (green:wan-1|red:wan) brightness=([01])', line)
        if led:
            leds[led[1]].append(int(led[2]))
        if line.startswith('protocol_error='):
            protocol.append(int(line.split('=', 1)[1]))
        if not line.startswith('{'):
            continue
        item = json.loads(line)
        if item.get('mode') == 'xgspon':
            controller.append(item)
        elif 'mib_objects' in item:
            omci.append(item)
        elif 'rx_bench' in item:
            receive.append(item)
    if len(omci) != count or len(controller) != count + 1 or protocol != [0] * count:
        raise ValueError(f'{capture}: incomplete {count}-sample observation')
    if (action == 'receive' or any(leds.values())) and any(len(values) != count for values in leds.values()):
        raise ValueError(f'{capture}: incomplete LED observation')
    if len(receive) != (count if action == 'receive' else 0):
        raise ValueError(f'{capture}: incomplete receive observation')
    for item in controller:
        if any(item.get(key) is not True for key in (
            'gpon_detected', 'xgspon_detected', 'md32_enabled', 'tx_disabled',
            'tx_inhibited', 'calibration_supplied', 'firmware_verified',
        )) or item.get('last_error') != 0:
            raise ValueError(f'{capture}: controller observation failed')
        if type(item.get('los')) is not bool or (fiber == 'disconnected' and not item['los']):
            raise ValueError(f'{capture}: controller LOS observation failed')
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
        'capture': str(capture), 'boot_revision': record['revision'], 'action': action, 'fiber': fiber,
        'observations': len(omci), 'controller_tx_disabled': True,
        'tx_inhibited': True, 'los': sorted({x['los'] for x in controller}), 'protocol_error': 0,
        'omci_state': 1, 'mib_objects': sorted({x['mib_objects'] for x in omci}),
        'service_error': 0, 'cleanup': 'passed',
        'serial_start': record['serial_start'],
        'elapsed_seconds': round(record['finished'] - record['started'], 3),
        'fiber_led_brightness': {key: sorted(set(values)) for key, values in leds.items()},
        'physical_led_requires_user_observation': True,
        'receive': receive_summary(receive, fiber, reacquire) if action == 'receive' else None,
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
        'schema_version': 2, 'result': 'bench observations and cleanup passed',
        'optical_service_verified': False, 'runs': runs,
    }, indent=2))


if __name__ == '__main__':
    main()
