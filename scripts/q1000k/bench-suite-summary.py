#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Independently validate saved suite captures and summarize hardware outcomes.

No SSH/device access. Partial summaries require --partial and are not acceptance.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path

SPEC=importlib.util.spec_from_file_location('suite',Path(__file__).with_name('bench-suite.py'))
SUITE=importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUITE)


def summarize(directory,partial=False):
    suite=json.loads((directory/'suite.json').read_text())
    if suite['status']!='collection-complete' and not partial:
        raise ValueError('Suite did not complete; use --partial to inspect completed cases only')
    rows=[]; powers=[]; count=0
    for completed in suite['results']:
        name=completed['name']
        case=next(c for c in suite['cases'] if c['name']==name)
        capture=directory/name
        observed=SUITE.REPORT.summarize(capture,allow_downstream_failure=True)
        SUITE.check_case(observed,case['probe'],case['reacquire'])
        receiver=SUITE.RECEIVER.summarize(capture)
        rx=observed['receive']; diag=observed['probe_diagnostics']
        samples=[]
        for line in (capture/'attempt.log').read_text().splitlines():
            if line.startswith('{'):
                sample=json.loads(line)
                if 'rx_bench' in sample: samples.append(sample)
        readings=[r['rx_power_nw'] for r in samples if r['rx_power_valid']]
        powers.extend(readings); count+=len(samples)
        attempted=[r for r in samples if r['reacquire_attempts']]
        before=[r for r in samples if not r['reacquire_attempts']]
        counters=('frames','lof','fec_total','fec_corrected','fec_uncorrected','irq_calls')
        rows.append(dict(name=name,samples=len(samples),attempts=diag['attempts'],
            post_attempt_samples=len(attempted),
            post_attempt_seconds=round((attempted[-1]['sampled_ms']-attempted[0]['sampled_ms'])/1000,3) if attempted else None,
            checked_probe_writes=diag['checked_writes'],probe_restored=diag['restoration_confirmed'],
            downstream_stable=rx['downstream_stable'],
            observed_sync=sorted({r['sync_status'] for r in samples}),
            synced_values=sorted({r['synced'] for r in samples}),
            counter_values={k:sorted({r[k] for r in samples}) for k in counters},
            pcs_counter_values={k:sorted({r['pcs_counters'][k] for r in samples}) for k in samples[0]['pcs_counters']},
            rx_power_nw=dict(min=min(readings),max=max(readings),last=readings[-1]) if readings else None,
            rx_power_last_dbm=round(10*math.log10(readings[-1]/1e6),2) if readings else None,
            receiver_before={k:sorted({r['receiver'][k] for r in before}) for k in ('rx_control','pcs_debug_control','rx_frontend_gain')},
            receiver_after={k:sorted({r['receiver'][k] for r in attempted}) for k in ('rx_control','pcs_debug_control','rx_frontend_gain')},
            rx_meter_window=diag['rx_meter_upper16_within_configured_window'],
            diagnostic_before=diag['before'],diagnostic_after=diag['after'],
            checker_error_phases=diag['checker_error_phases'],
            cleanup=observed['cleanup']))
    if suite['status']=='collection-complete' and [r['name'] for r in rows]!=[c['name'] for c in suite['cases']]:
        raise ValueError('Completed suite does not contain every planned case in order')
    return dict(schema_version=1,suite=str(directory),status=suite['status'],device_access=False,
        cases_validated=len(rows),samples=count,optical_service_verified=False,
        sync_seen=any(True in r['synced_values'] for r in rows),
        any_pcs_or_frame_counter_nonzero=any(any(v for key,values in r['counter_values'].items() if key!='irq_calls' for v in values)
                                       or any(v for values in r['pcs_counter_values'].values() for v in values) for r in rows),
        rx_power_nw=dict(min=min(powers),max=max(powers),last=powers[-1]) if powers else None,
        results=rows,limits=['Software trials test specific settings and sequences, not every combination.',
                           'LOS/power and frequency-monitor counts do not establish wavelength, absolute calibration, electrical polarity or recovered-data quality.',
                           'Physical controls in the coverage guide remain pending unless separately recorded.'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--partial',action='store_true')
    args=parser.parse_args()
    print(json.dumps(summarize(args.directory.resolve(),args.partial),indent=2))

if __name__=='__main__': main()
