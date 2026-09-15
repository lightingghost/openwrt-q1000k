#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate and summarize all RX probe diagnostics, without device access."""
import argparse
import json
from pathlib import Path
import re

PROBES = ('bit-order', 'descrambler', 'fec-oc', 'fec-off', 'gain-auto', 'gain-low',
          'tdc-delay', 'pll-order', 'oem-order', 'checker')
HEADER = Path(__file__).resolve().parents[2] / 'package/kernel/airoha-pon/src/bsp/include/q1000k_rx_diag.h'
FIELDS = tuple(re.findall(r'X\((\w+),', HEADER.read_text()))


def summarize(capture):
    record = json.loads((capture/'checkpoint.json').read_text())
    if record.get('diagnostics_version') != 1:
        raise ValueError('Capture requires diagnostics schema 1')
    probe = record.get('probe')
    if probe is not None and probe not in PROBES:
        raise ValueError('Unknown probe in checkpoint')
    mode = PROBES.index(probe)+1 if probe else 0
    if mode and (not record.get('reacquire_once') or record.get('restore_gain') or record.get('restore_pll')):
        raise ValueError('Probe request is not exclusive recovery')
    rows, rx = [], []
    for line in (capture/'attempt.log').read_text().splitlines():
        if not line.startswith('{'):
            continue
        item = json.loads(line)
        if 'diagnostics_version' in item:
            if len(rx) != len(rows)+1:
                raise ValueError('Probe diagnostics are not paired with RX samples')
            rows.append(item)
        elif 'rx_bench' in item:
            rx.append(item)
    count = record.get('samples', 30)
    if len(rows) != count or len(rx) != count:
        raise ValueError('Incomplete probe diagnostic capture')
    previous_ms, attempts, writes = -1, 0, 0
    for row, sample in zip(rows,rx):
        expected = set(FIELDS) | {'diagnostics_version','probe','attempts','writes','sampled_ms'}
        if row.keys() != expected or any(type(v) is not int or v<0 for v in row.values()):
            raise ValueError('Invalid diagnostic fields/types')
        if any(row[k]>0xffffffff for k in FIELDS):
            raise ValueError('Diagnostic register overflows u32')
        if row['diagnostics_version']!=1 or row['probe']!=mode:
            raise ValueError('Diagnostic mode/schema mismatch')
        if not previous_ms < row['sampled_ms'] or not 0 <= row['sampled_ms']-sample['sampled_ms'] <= 1500:
            raise ValueError('Stale/unpaired diagnostics')
        if not max(attempts,sample['reacquire_attempts']) <= row['attempts'] <= int(record.get('reacquire_once',False)):
            raise ValueError('Invalid probe attempt count')
        if row['writes'] < writes or (row['writes'] and (not mode or not row['attempts'])):
            raise ValueError('Invalid probe write count')
        if mode and row['attempts'] and not row['writes']:
            raise ValueError('Probe attempt has no checked write')
        if row['checker_control'] & 256 or row['data_route_control'] & 65792 or row['bist_lane_control'] & 256:
            raise ValueError('Upstream test generator or loopback is active')
        previous_ms, attempts, writes = row['sampled_ms'],row['attempts'],row['writes']
    serial = (capture/'serial.log').read_text()
    if mode and attempts and serial.count('q1000k: RX probe fields restored')!=1:
        raise ValueError('Probe field restoration was not confirmed exactly once')
    def words(items):
        return {key:[f'0x{x:08x}' for x in sorted({row[key] for row in items})] for key in FIELDS}
    def counter_delta(items):
        return dict(samples=len(items), first=items[0]['checker_errors'] if items else None,
                    last=items[-1]['checker_errors'] if items else None,
                    delta_mod32=(items[-1]['checker_errors']-items[0]['checker_errors']) & 0xffffffff if len(items)>1 else None)
    before=[r for r in rows if not r['attempts']]
    after=[r for r in rows if r['attempts']]
    meter = []
    for row in rows:
        target=row['rx_meter_lock_target']; result=row['rx_meter_result']>>16
        meter.append((target & 0xffff) <= result <= (target>>16))
    return dict(schema_version=1, capture=str(capture), probe=probe, attempts=attempts,
                checked_writes=writes, samples=len(rows), restoration_confirmed=bool(mode and attempts),
                raw_words=words(rows), before=words([r for r in rows if not r['attempts']]),
                after=words([r for r in rows if r['attempts']]),
                rx_meter_upper16_within_configured_window=sorted(set(meter)),
                checker_event_values=sorted({r['checker_event'] for r in rows}),
                checker_error_delta_mod32=None if attempts else counter_delta(rows)['delta_mod32'],
                checker_error_phases=dict(before=counter_delta(before),after=counter_delta(after)),
                counter_delta_crosses_intervention=False,
                limits=['Frequency words are not a calibrated baud-rate measurement or CDR-lock proof.',
                        'Normal XGS-PON is not PRBS: checker errors are not an optical BER measurement.',
                        'No counter delta is computed across recovery/checker restart, which may reset the counter.',
                        'Power/LOS cannot prove wavelength, modulation quality, differential wiring or absolute calibration.'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path)
    args=parser.parse_args()
    print(json.dumps(summarize(args.capture),indent=2))

if __name__=='__main__':
    main()
