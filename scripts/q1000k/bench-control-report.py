#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate a saved physical RX control and group observations by LOS/attempt.

Reads host captures only. Mixed LOS is allowed, but all normal teardown,
TX-inhibit and diagnostic checks remain mandatory. No optical acceptance is
inferred from a completed capture or a latched checker result.
"""
import argparse
import importlib.util
import itertools
import json
from pathlib import Path

SPEC = importlib.util.spec_from_file_location('suite', Path(__file__).with_name('bench-suite.py'))
SUITE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUITE)


def summarize(capture):
    observed = SUITE.REPORT.summarize(capture, allow_downstream_failure=True)
    receiver = SUITE.RECEIVER.summarize(capture)
    if not observed['receive'] or not observed['probe_diagnostics']:
        raise ValueError('Physical control requires RX and probe diagnostics')
    rx, diagnostics = [], []
    for line in (capture/'attempt.log').read_text().splitlines():
        if not line.startswith('{'):
            continue
        row = json.loads(line)
        if 'rx_bench' in row:
            rx.append(row)
        if 'diagnostics_version' in row:
            diagnostics.append(row)
    phases = []
    numbered = list(enumerate(zip(rx, diagnostics), 1))
    def phase_key(item):
        _, (sample, diag) = item
        return sample['controller_los'], sample['phy_los'], diag['attempts']
    for (controller_los, phy_los, attempts), group in itertools.groupby(numbered, phase_key):
        group = list(group)
        samples = [item[1][0] for item in group]
        diags = [item[1][1] for item in group]
        counters = ('frames', 'lof', 'fec_total', 'fec_corrected', 'fec_uncorrected', 'irq_calls')
        phases.append(dict(
            first_sample=group[0][0], last_sample=group[-1][0], samples=len(group),
            first_ms=samples[0]['sampled_ms'], last_ms=samples[-1]['sampled_ms'],
            duration_seconds=round((samples[-1]['sampled_ms']-samples[0]['sampled_ms'])/1000, 3),
            controller_los=controller_los, phy_los=phy_los, attempts=attempts,
            optical=SUITE.REPORT.optical_summary(samples),
            synced=sorted({r['synced'] for r in samples}),
            counters={k:sorted({r[k] for r in samples}) for k in counters},
            pcs_counters={k:sorted({r['pcs_counters'][k] for r in samples}) for k in samples[0]['pcs_counters']},
            diagnostics={k:sorted({r[k] for r in diags}) for k in (
                'attempts', 'writes', 'checker_control', 'checker_event', 'checker_errors',
                'rx_meter_result', 'jcpll_500m_result', 'bist_lane_control', 'data_route_control',
                'tdc_ncpo', 'fifo_clock_status') if k in diags[0]},
            passive_clock_words={k:dict(first=diags[0][k], last=diags[-1][k],
                minimum=min(r[k] for r in diags), maximum=max(r[k] for r in diags),
                distinct_values=len({r[k] for r in diags}))
                for k in ('tdc_ncpo', 'fifo_clock_status') if k in diags[0]},
            analog_fields=SUITE.RECEIVER.analog_fields([r['receiver'] for r in samples]),
            sfp_status=sorted({r['receiver']['sfp_status'] for r in samples})))
    report = dict(schema_version=1, capture=str(capture), device_access=False,
                  capture_validated=True, optical_service_verified=False,
                  samples=len(rx), cleanup=observed['cleanup'], phases=phases,
                  limits=['Phases follow sampled LOS and attempt state, not exact physical transition time.',
                          'Checker completion/errors may be latched; unchanged values do not prove live data.',
                          'Reconnection is demonstrated only if a light phase follows a dark phase in this capture.',
                          'NCPO is a raw tracking word; passive FIFO status may be stale without a latch write.',
                          'Passive DAC/offset codes may be stale without a latch; FIFO nibbles do not prove data continuity or clock lock.',
                          'An upper frequency count in its target window is not calibrated rate or recovered-clock proof.'])
    return report, observed, receiver


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--write', action='store_true', help='Save derived reports beside the raw capture')
    args = parser.parse_args()
    capture = args.capture.resolve()
    report, observed, receiver = summarize(capture)
    if args.write:
        for name, data in (
                ('control-report.json', report), ('observations.json', observed),
                ('receiver-report.json', receiver), ('probe-report.json', observed['probe_diagnostics']),
                ('hypotheses.json', SUITE.HYPOTHESES.evaluate(receiver))):
            (capture/name).write_text(json.dumps(data, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
