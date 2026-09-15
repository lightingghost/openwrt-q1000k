#!/usr/bin/env python3
"""Verify saved stack/receive bench observations and cleanup; no SSH access."""
import argparse
import json
import math
import re
from pathlib import Path


def optical_summary(samples):
    present = ['rx_power_valid' in x or 'rx_power_nw' in x or
               x.get('receiver_version', 1) >= 4 for x in samples]
    if not any(present):
        return {'source': 'controller', 'available_samples': 0, 'unavailable_samples': len(samples),
                'rx_power_nw': None, 'rx_power_dbm': None}
    if not all(present):
        raise ValueError('Incomplete RX power telemetry')
    readings = []
    for row in samples:
        valid, power = row.get('rx_power_valid'), row.get('rx_power_nw')
        if type(valid) is not bool:
            raise ValueError('Invalid RX power validity flag')
        if valid:
            if type(power) is not int or not 100 <= power <= 6553400 or power % 100:
                raise ValueError('Invalid RX optical power')
            readings.append(power)
        elif 'rx_power_nw' not in row or power is not None:
            raise ValueError('Unavailable RX power must be null')
    def bounds(values):
        return dict(first=values[0], last=values[-1], minimum=min(values), maximum=max(values)) if values else None
    return dict(source='controller', available_samples=len(readings),
                unavailable_samples=len(samples)-len(readings), rx_power_nw=bounds(readings),
                rx_power_dbm=bounds([round(10 * math.log10(x / 1000000), 2) for x in readings]))


def receive_summary(samples, fiber, reacquire=False, require_stability=True, restore_pll=False, restore_gain=False):
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
        if restore_gain or 'gain_restore_enabled' in item or item.get('receiver_version', 1) >= 4:
            if item.get('gain_restore_enabled') is not restore_gain:
                raise ValueError('Receiver gain restoration guard failed')
        if restore_pll or 'pll_restore_enabled' in item or item.get('receiver_version', 1) >= 3:
            if item.get('pll_restore_enabled') is not restore_pll:
                raise ValueError('PLL restoration guard failed')
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
    if not last_poll:
        raise ValueError('Receive polling did not run')
    downstream_stable = fiber == 'connected' and stable >= 5
    if require_stability and fiber == 'connected' and not downstream_stable:
        raise ValueError('Receive polling or final downstream stability failed')
    return {
        'samples': len(samples), 'registration_enabled': False, 'mac_irq_mask': 0,
        'final_stable_intervals': stable,
        'downstream_stable': downstream_stable,
        'counters': {key: {'first': samples[0][key], 'last': samples[-1][key]}
                     for key in ('sampled_ms', 'frames', 'lof', 'fec_total', 'fec_corrected',
                                 'fec_uncorrected', 'irq_calls', 'poll_calls')},
        'controller_los': sorted({x['controller_los'] for x in samples}),
        'phy_los': sorted({x['phy_los'] for x in samples}),
        'synced': sorted({x['synced'] for x in samples}),
        'reacquire_requested': reacquire, 'reacquire_attempts': last_attempts,
        'pll_restore_requested': restore_pll, 'gain_restore_requested': restore_gain,
        'optical': optical_summary(samples),
    }


def summarize(capture, allow_downstream_failure=False):
    capture = capture.resolve(strict=True)
    record = json.loads((capture / 'checkpoint.json').read_text())
    for field, expected in {
        'schema_version': 1, 'host': '192.168.255.1',
        'postflight': 'passed', 'input_cleanup': 'passed',
    }.items():
        if record.get(field) != expected:
            raise ValueError(f'{capture}: {field} is not {expected!r}')
    action, fiber = record.get('action'), record.get('fiber')
    failed_downstream = record.get('status') == 'failed' and allow_downstream_failure
    if record.get('status') != 'passed' and not failed_downstream:
        raise ValueError('Capture did not pass')
    attempt = (capture / 'attempt.log').read_text()
    if failed_downstream:
        # Matrix continuation accepts only this complete observational failure.
        # Every controller, MAC, OMCI, TX, serial and cleanup guard below still
        # has to pass; an SSH timeout or partial capture cannot advance it.
        failures = [x for x in attempt.splitlines() if x.startswith('Q1000K bench:')]
        if (action != 'receive' or fiber != 'connected' or failures != [
                'Q1000K bench: Downstream LOS/sync/frame stability was not established.'] or
                not record.get('error', '').startswith('SSH failed (1);')):
            raise ValueError('Capture failure is not limited to downstream stability')
    restore_gain = record.get('restore_gain', False)
    restore_pll = record.get('restore_pll', False)
    reacquire = record.get('reacquire_once', False)
    if type(restore_gain) is not bool or (restore_gain and not reacquire):
        raise ValueError('Invalid receiver gain restoration request')
    if type(restore_pll) is not bool or (restore_pll and not reacquire):
        raise ValueError('Invalid PLL restoration request')
    if type(reacquire) is not bool or (reacquire and (action != 'receive' or fiber != 'connected')):
        raise ValueError('Invalid receive reacquisition request')
    if action not in ('stack', 'receive') or fiber not in ('connected', 'disconnected'):
        raise ValueError('Unknown bench action/fiber state')
    if action == 'stack' and fiber != 'disconnected':
        raise ValueError('Normal stack test requires disconnected fiber')
    count = record.get('samples', 30) if action == 'receive' else 5
    if type(count) is not int or (action == 'receive' and count not in (30, 90, 180)):
        raise ValueError('Invalid receive observation count')
    controller, omci, protocol, receive = [], [], [], []
    leds = {'green:wan-1': [], 'red:wan': []}
    for line in attempt.splitlines():
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
    rx = receive_summary(receive, fiber, reacquire, not failed_downstream, restore_pll, restore_gain) if action == 'receive' else None
    if restore_gain and rx['reacquire_attempts'] and len(re.findall(
            r'q1000k: RX gain restored to (?:0x[0-9a-f]+|0)\b', serial)) != 1:
        raise ValueError('Original receiver gain restoration was not confirmed')
    if failed_downstream and rx['downstream_stable']:
        raise ValueError('Downstream failure disagrees with the captured observations')
    return {
        'bench_result': record['status'],
        'capture': str(capture), 'boot_revision': record['revision'], 'action': action, 'fiber': fiber,
        'observations': len(omci), 'controller_tx_disabled': True,
        'tx_inhibited': True, 'los': sorted({x['los'] for x in controller}), 'protocol_error': 0,
        'omci_state': 1, 'mib_objects': sorted({x['mib_objects'] for x in omci}),
        'service_error': 0, 'cleanup': 'passed',
        'serial_start': record['serial_start'],
        'elapsed_seconds': round(record['finished'] - record['started'], 3),
        'fiber_led_brightness': {key: sorted(set(values)) for key, values in leds.items()},
        'physical_led_requires_user_observation': True,
        'receive': rx,
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
