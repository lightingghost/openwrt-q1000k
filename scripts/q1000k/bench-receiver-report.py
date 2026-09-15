#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Summarize complete RX diagnostic captures, retaining a failed bench result.

Reads saved logs only. This is not the passing-bench acceptance report.
"""
import argparse
import json
from pathlib import Path

PHY_WORDS = ('rx_control', 'pcs_reset', 'pma_reset', 'clock_control',
             'cdr_control', 'rx_frequency', 'pll_status', 'tdc_control',
             'rx_analog0', 'rx_analog1', 'rx_analog2',
             'rx_sequence_force', 'rx_sequence_disable')
CONTROLLER_WORDS = ('mcu_a0', 'mcu_a2', 'apd_control', 'ocp_control',
                    'firmware_status', 'los_control', 'system_status')
EXTENDED_PHY_WORDS = ('rx_sequence_force0', 'rx_sequence_disable0',
                      'rx_lock_force', 'rx_lock_disable', 'rx_oscal_control',
                      'rx_reset0', 'rx_reset1', 'pll_power', 'pll_filter',
                      'pll_pcw1', 'pll_pcw2')
COUNTERS = ('sampled_ms', 'frames', 'lof', 'fec_total', 'fec_corrected',
            'fec_uncorrected', 'irq_calls', 'poll_calls')


def words(rows, names):
    for row in rows:
        for name in names:
            value = row.get(name)
            if type(value) is not int or not 0 <= value < 0xffffffff:
                raise ValueError('Invalid receiver word: ' + name)
    return {name: [f'0x{value:08x}' for value in sorted({x[name] for x in rows})]
            for name in names}


def summarize(capture):
    capture = capture.resolve(strict=True)
    record = json.loads((capture / 'checkpoint.json').read_text())
    if (record.get('schema_version') != 1 or record.get('action') != 'receive' or
            record.get('host') != '192.168.255.1' or
            record.get('fiber') not in ('connected', 'disconnected') or
            record.get('status') not in ('passed', 'failed')):
        raise ValueError('A completed receive capture is required')
    if record.get('postflight') != 'passed' or record.get('input_cleanup') != 'passed':
        raise ValueError('Capture cleanup has not been verified')
    objects = [json.loads(x) for x in (capture / 'attempt.log').read_text().splitlines()
               if x.startswith('{')]
    rx = [x for x in objects if x.get('rx_bench') is True]
    controller = [x for x in objects if x.get('receiver_status') is True]
    count = record.get('samples', 30)
    if type(count) is not int or count not in (30, 90, 180):
        raise ValueError('Invalid receive observation count')
    if len(rx) != count or len(controller) != count + 1:
        raise ValueError(f'Expected {count} RX and {count + 1} controller snapshots')
    versions = {x.get('receiver_version', 1) for x in rx}
    if versions not in ({1}, {2}) or any(type(x.get('receiver_version', 1)) is not int for x in rx):
        raise ValueError('Inconsistent receiver diagnostic version')
    names = PHY_WORDS + (EXTENDED_PHY_WORDS if versions == {2} else ())
    last = -1
    last_attempts = 0
    reacquire = record.get('reacquire_once', False)
    if type(reacquire) is not bool or (reacquire and record['fiber'] != 'connected'):
        raise ValueError('Invalid receive reacquisition request')
    for item in rx:
        for key, value in {'tx_inhibited': True, 'tx_enabled': False,
                           'registration_enabled': False}.items():
            if item.get(key) is not value:
                raise ValueError('Receive guard failed: ' + key)
        if type(item.get('mac_irq_mask')) is not int or item['mac_irq_mask'] != 0:
            raise ValueError('MAC interrupts were not masked')
        for key in COUNTERS + ('sync_status',):
            if type(item.get(key)) is not int or item[key] < 0:
                raise ValueError('Invalid receive counter: ' + key)
        for key in ('controller_los', 'phy_los', 'synced'):
            if type(item.get(key)) is not bool:
                raise ValueError('Invalid receive flag: ' + key)
        if item['sampled_ms'] <= last:
            raise ValueError('Stale receive sample')
        last = item['sampled_ms']
        if reacquire or 'reacquire_enabled' in item or 'reacquire_attempts' in item:
            attempts = item.get('reacquire_attempts')
            if (item.get('reacquire_enabled') is not reacquire or type(attempts) is not int or
                    not last_attempts <= attempts <= int(reacquire) or
                    (attempts and item['poll_calls'] < 10)):
                raise ValueError('Receive reacquisition guard failed')
            last_attempts = attempts
    return {
        'schema_version': 1, 'capture': str(capture), 'revision': record['revision'],
        'bench_result': record['status'], 'bench_error': record.get('error'),
        'report_kind': 'diagnostic observations, not service acceptance',
        'optical_service_verified': False, 'fiber': record['fiber'],
        'postflight': record['postflight'], 'input_cleanup': record['input_cleanup'],
        'rx_samples': len(rx), 'controller_samples': len(controller),
        'tx_inhibited': True, 'tx_enabled': False, 'registration_enabled': False,
        'mac_irq_mask': 0,
        'receiver_version': next(iter(versions)),
        'reacquire_requested': reacquire, 'reacquire_attempts': last_attempts,
        'phy_words_before_reacquire': words([x['receiver'] for x in rx
                                            if x.get('reacquire_attempts', 0) == 0], names),
        'phy_words_after_reacquire': words([x['receiver'] for x in rx
                                           if x.get('reacquire_attempts', 0) == 1], names),
        'rx_states': {k: sorted({x[k] for x in rx}) for k in
                      ('controller_los', 'phy_los', 'synced', 'sync_status')},
        'counters': {k: {'first': rx[0][k], 'last': rx[-1][k]} for k in COUNTERS},
        'phy_words': words([x['receiver'] for x in rx], names),
        'controller_words': words(controller, CONTROLLER_WORDS),
        'elapsed_seconds': round(record['finished'] - record['started'], 3),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    try:
        result = summarize(args.capture)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'Cannot summarize receiver diagnostics: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
