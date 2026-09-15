#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read partial suite or single captures; never contact the bench device.

This is not acceptance validation. Use the completed suite/report artifacts.
A partially written JSON line is ignored and explicitly reported.
"""
import argparse
import json
import math
from pathlib import Path


def progress(directory):
    if not (directory/'suite.json').exists():
        return dict(acceptance_validated=False, current=capture_progress(directory))
    suite=json.loads((directory/'suite.json').read_text())
    completed=[r['name'] for r in suite['results']]
    result=dict(status=suite['status'],completed=completed,
                acceptance_validated=False,error=suite.get('error'))
    for case in suite['cases']:
        capture=directory/case['name']
        checkpoint=capture/'checkpoint.json'
        if case['name'] in completed or not checkpoint.exists():
            continue
        result['current']=capture_progress(capture)
        break
    return result


def capture_progress(capture):
    record=json.loads((capture/'checkpoint.json').read_text())
    rx,diagnostics,partial=[],[],False
    attempt=capture/'attempt.log'
    for line in attempt.read_text().splitlines() if attempt.exists() else []:
        if not line.startswith('{'):
            continue
        try: row=json.loads(line)
        except json.JSONDecodeError:
            partial=True
            continue
        if 'rx_bench' in row: rx.append(row)
        if 'diagnostics_version' in row: diagnostics.append(row)
    live=dict(name=capture.name,status=record['status'],samples=len(rx),
              expected_samples=record.get('samples',30),partial_json=partial)
    if rx:
        row=rx[-1]
        live.update({key:row.get(key) for key in (
            'controller_los','phy_los','synced','frames','lof','fec_total',
            'irq_calls','poll_calls','reacquire_attempts','rx_power_nw')})
        nw=row.get('rx_power_nw')
        live['rx_power_dbm']=round(10*math.log10(nw/1e6),2) if nw and row.get('rx_power_valid') else None
    if diagnostics:
        row=diagnostics[-1]
        live['diagnostics']={k:row.get(k) for k in (
            'probe','attempts','writes','rx_meter_result',
            'checker_control','checker_event','checker_errors')}
    return live


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    args=parser.parse_args()
    try: data=progress(args.directory)
    except json.JSONDecodeError:
        data=dict(status='capture-metadata-being-written',acceptance_validated=False)
    print(json.dumps(data,indent=2))

if __name__=='__main__': main()
