#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate and summarize all RX probe diagnostics, without device access."""
import argparse
import json
from pathlib import Path
import re

PROBES = ('bit-order', 'descrambler', 'fec-oc', 'fec-off', 'gain-auto', 'gain-low',
          'tdc-delay', 'pll-order', 'oem-order', 'checker',
          'cdr-auto-release', 'cdr-internal-auto', 'prcal-finalize', 'fll-auto',
          'rx-sequence-auto', 'post-eye-ready', 'oem-clock-cycle', 'oem-rx-acquire',
          'oem-peaking', 'checker-dark', 'combined-auto', 'prcal-rerun',
          'eye-current', 'oem-analog', 'oem-full-reset', 'oem-cal-reset', 'oem-cal-auto', 'oem-eye-0', 'oem-eye-1', 'oem-eye-2', 'oem-eye-3', 'oem-eye-4', 'oem-eye-5', 'oem-eye-6', 'oem-eye-7', 'oem-post-init', 'oem-post-cal')
HEADER = Path(__file__).resolve().parents[2] / 'package/kernel/airoha-pon/src/bsp/include/q1000k_rx_diag.h'
FIELDS = tuple(re.findall(r'X\((\w+),', HEADER.read_text()))
V2_FIELDS = ('tdc_ncpo', 'fifo_clock_status')
V3_FIELDS = ('cdr_injection', 'cdr_lpf_override', 'fll_idac', 'fll_load',
             'eye_reset_force', 'eye_reset_mode', 'eye_pi_ready', 'eye_pi_mode',
             'eye_count_ready', 'eye_count_mode', 'rx_peaking_control')
V4_FIELDS = ('eye_pi_raw', 'eye_ready_raw', 'eye_done_raw', 'eye_horizontal_raw', 'eye_vertical_raw', 'eye_latch_control', 'fll_adc_raw0', 'fll_adc_raw1', 'fll_adc_raw2', 'fll_adc_raw3', 'fll_adc_raw4')
FIELDS_BY_VERSION = {1: tuple(k for k in FIELDS if k not in V2_FIELDS + V3_FIELDS + V4_FIELDS),
                     2: tuple(k for k in FIELDS if k not in V3_FIELDS + V4_FIELDS),
                     3: tuple(k for k in FIELDS if k not in V4_FIELDS), 4: FIELDS}


def eye_observation(serial, probe, attempts):
    """A fresh measurement is identified by its bounded probe, never by a
    passive status word that might still contain an earlier latch value.
    """
    expected = bool(attempts and (probe in ('eye-current', 'oem-analog', 'oem-cal-reset',
                                          'oem-cal-auto', 'oem-post-cal') or
                                  (probe or '').startswith('oem-eye-')))
    keys = ('pi', 'done', 'ready', 'horizontal', 'vertical', 'dac0', 'dac1')
    pattern = 'q1000k: RX eye fresh=1 ' + ' '.join(key + '=([0-9a-f]{8})' for key in keys)
    matches = re.findall(pattern, serial)
    if len(matches) != int(expected) or serial.count('q1000k: RX eye fresh=') != int(expected):
        raise ValueError('Fresh eye observation is missing, duplicated or unexpected')
    if not expected:
        return None
    raw = dict(zip(keys, (int(word, 16) for word in matches[0])))
    flags = {name: bool(raw[word] & (1 << bit)) for name, word, bit in
             (('x_done', 'done', 16), ('y_done', 'done', 24),
              ('horizontal_ready', 'ready', 16), ('vertical_ready', 'ready', 24))}
    valid = all(flags.values())
    def signed7(value):
        value &= 127
        return value - 128 if value & 64 else value
    left, right = (raw['horizontal'] >> 16) & 2047, raw['horizontal'] & 2047
    top, bottom = signed7(raw['vertical']), signed7(raw['vertical'] >> 8)
    return dict(fresh=True, raw=raw, completion=flags, completed=valid,
                horizontal_ticks=abs(left-right) if valid else None,
                vertical_ticks=abs(top-bottom) if valid else None,
                endpoints=dict(left=left, right=right, top=top, bottom=bottom),
                data_lock_proven=False, optical_ber_measured=False,
                limits=['Raw internal eye ticks have no calibrated voltage/time scale.',
                        'Completion and a nonzero opening alone do not prove valid downstream data.',
                        'A failed completion is retained as an observation; there is no hidden recovery retry.'])


def summarize(capture):
    record = json.loads((capture/'checkpoint.json').read_text())
    version = record.get('diagnostics_version')
    if type(version) is not int or version not in FIELDS_BY_VERSION:
        raise ValueError('Capture requires diagnostics schema 1, 2, 3 or 4')
    fields = FIELDS_BY_VERSION[version]
    probe = record.get('probe')
    if probe is not None and probe not in PROBES:
        raise ValueError('Unknown probe in checkpoint')
    mode = PROBES.index(probe)+1 if probe else 0
    if mode > 22 and version < 4:
        raise ValueError("Deep receiver probe requires diagnostic schema 4")
    if mode > 10 and version < 3:
        raise ValueError('Acquisition probe requires diagnostic schema 3')
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
        expected = set(fields) | {'diagnostics_version','probe','attempts','writes','sampled_ms'}
        if row.keys() != expected or any(type(v) is not int or v<0 for v in row.values()):
            raise ValueError('Invalid diagnostic fields/types')
        if any(row[k]>0xffffffff for k in fields):
            raise ValueError('Diagnostic register overflows u32')
        if row['diagnostics_version']!=version or row['probe']!=mode:
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
    eye = eye_observation(serial, probe, attempts)
    post_expected = bool(attempts and probe in ('oem-post-init', 'oem-post-cal'))
    if serial.count('q1000k: RX OEM post-init checked=1 mask=00000100 value=00000100') != int(post_expected):
        raise ValueError('OEM controller post-init verification marker mismatch')
    if probe == 'checker-dark' and attempts:
        first = next(i for i, row in enumerate(rows) if row['attempts'])
        if (not rx[first].get('controller_los') or not rx[first].get('phy_los') or
                not any(sample.get('controller_los') is False and sample.get('phy_los') is False
                        for sample in rx[:first])):
            raise ValueError('Fresh dark checker lacks observed light followed by a dark first attempt')
    if mode and attempts and serial.count('q1000k: RX probe fields restored')!=1:
        raise ValueError('Probe field restoration was not confirmed exactly once')
    def words(items):
        return {key:[f'0x{x:08x}' for x in sorted({row[key] for row in items})] for key in fields}
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
    return dict(schema_version=1, diagnostics_version=version, capture=str(capture), probe=probe, attempts=attempts,
                fresh_eye=eye, oem_post_init_verified=post_expected,
                checked_writes=writes, samples=len(rows), restoration_confirmed=bool(mode and attempts),
                raw_words=words(rows), before=words([r for r in rows if not r['attempts']]),
                after=words([r for r in rows if r['attempts']]),
                rx_meter_upper16_within_configured_window=sorted(set(meter)),
                checker_event_values=sorted({r['checker_event'] for r in rows}),
                checker_error_delta_mod32=None if attempts else counter_delta(rows)['delta_mod32'],
                checker_error_phases=dict(before=counter_delta(before),after=counter_delta(after)),
                counter_delta_crosses_intervention=False,
                passive_clock_words={k: dict(first=rows[0][k], last=rows[-1][k],
                    minimum=min(r[k] for r in rows), maximum=max(r[k] for r in rows),
                    distinct_values=len({r[k] for r in rows})) for k in V2_FIELDS} if version >= 2 else None,
                limits=['Frequency words are not a calibrated baud-rate measurement or CDR-lock proof.',
                        'NCPO is a raw tracking word, not a lock flag or a calibrated frequency.',
                        'FIFO clock status is passive and may be stale without a latch; no latch/clear was written.',
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
