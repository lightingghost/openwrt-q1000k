#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compare completed matrix captures offline; never contacts the bench."""
import argparse
import importlib.util
import json
from pathlib import Path

SPEC = importlib.util.spec_from_file_location('bench_matrix', Path(__file__).with_name('bench-matrix.py'))
MATRIX = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MATRIX)


def transition(rows):
    """Describe observations around the one attempt, without attributing cause."""
    after = [i for i, row in enumerate(rows) if row.get('reacquire_attempts') == 1]
    if not after:
        return None
    index = after[0]
    if index == 0:
        raise ValueError('Reacquisition has no captured baseline')
    before, first, last = rows[index - 1], rows[index], rows[-1]
    if first['sampled_ms'] <= before['sampled_ms'] or first['sampled_ms'] < rows[0]['sampled_ms']:
        raise ValueError('Reacquisition timestamps are not ordered')
    changes = {key: {'last_before': f'0x{value:08x}',
                     'first_after': f'0x{first["receiver"][key]:08x}',
                     'final': f'0x{last["receiver"][key]:08x}'}
               for key, value in before['receiver'].items()
               if value != first['receiver'][key] or value != last['receiver'][key]}
    return dict(first_observed_sample=index + 1, poll_calls=first['poll_calls'],
                observed_after_ms=first['sampled_ms'] - rows[0]['sampled_ms'],
                post_attempt_observation_ms=last['sampled_ms'] - first['sampled_ms'],
                changed_phy_words=changes,
                interpretation='Observed changes around recovery; not proof of cause or clock lock')


def summarize(capture):
    capture = capture.resolve(strict=True)
    record = json.loads((capture / 'matrix.json').read_text())
    if record.get('schema_version') != 1 or record.get('status') != 'completed' or len(record.get('stages', [])) != 2:
        raise ValueError('A completed two-stage matrix is required')
    restore_gain = record.get('restore_gain', False)
    if type(restore_gain) is not bool:
        raise ValueError('Invalid matrix receiver gain request')
    restore_pll = record.get('restore_pll', False)
    if type(restore_pll) is not bool:
        raise ValueError('Invalid matrix PLL restoration request')
    baseline = MATRIX.REPORT.summarize(capture / 'baseline', allow_downstream_failure=True)
    name, reacquire = MATRIX.followup(baseline)
    runs = []
    for i, stage in enumerate(('baseline', name)):
        path = capture / stage
        report = MATRIX.REPORT.summarize(path, allow_downstream_failure=True)
        diagnostic = MATRIX.RECEIVER.summarize(path)
        count = report['observations']
        if ((count != 30 if i == 0 else count not in (90, 180)) or
                report['boot_revision'] != baseline['boot_revision'] or
                report['receive']['reacquire_requested'] is not (reacquire if i else False) or
                report['receive']['pll_restore_requested'] is not (restore_pll and reacquire if i else False) or
                report['receive']['gain_restore_requested'] is not (restore_gain and reacquire if i else False)):
            raise ValueError('Stage does not match the baseline/continuation plan')
        objects = [json.loads(line) for line in (path / 'attempt.log').read_text().splitlines()
                   if line.startswith('{')]
        rows = [row for row in objects if row.get('rx_bench') is True]
        runs.append(dict(stage=stage, bench_result=report['bench_result'], samples=count,
                         downstream_stable=report['receive']['downstream_stable'],
                         reacquire_attempts=diagnostic['reacquire_attempts'],
                         pll_restore_requested=diagnostic['pll_restore_requested'],
                         gain_restore_requested=diagnostic['gain_restore_requested'], optical=diagnostic['optical'],
                         rx_states=diagnostic['rx_states'], counters=diagnostic['counters'],
                         transition=transition(rows), controller_words=diagnostic['controller_words'],
                         cleanup=report['cleanup'], elapsed_seconds=report['elapsed_seconds']))
    return dict(schema_version=1, capture=str(capture), revision=baseline['boot_revision'],
                report_kind='Validated observations, not optical service acceptance',
                optical_service_verified=False, tx_inhibited=True, tx_enabled=False,
                registration_enabled=False, device_access=False, stages=runs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    try:
        result = summarize(args.capture)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'Cannot compare matrix: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
