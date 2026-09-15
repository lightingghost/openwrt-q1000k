#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run two bounded connected RX experiments on one matching RAM boot.

No flashing, reboot, optical TX, normal registration or module substitution.
All diagnostics share each observation window; PHY experiments are sequential.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import time


def module(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'),
                                                Path(__file__).with_name(name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


RUN = module('bench-run')
REPORT = module('bench-report')
RECEIVER = module('bench-receiver-report')


def followup(baseline):
    """Only a complete, safe observation may inform the second experiment."""
    rx = baseline['receive']
    if (baseline['cleanup'] != 'passed' or baseline['fiber'] != 'connected' or
            rx['reacquire_requested'] or rx['reacquire_attempts']):
        raise ValueError('Baseline is not a clean connected observation')
    if rx['downstream_stable']:
        return 'extended-observe', False
    # Lost/uncertain light would make recovery a different experiment.
    if rx['controller_los'] != [False] or rx['phy_los'] != [False]:
        raise ValueError('Light was not consistently detected; stop to check the connection')
    return 'single-reacquire', True


def stage(args, name, samples, reacquire):
    capture = args.output / name
    command = ['receive', '--artifact', str(args.artifact), '--output', str(capture),
               '--inputs', str(args.inputs), '--fiber-connected', '--samples', str(samples),
               '--serial-log', str(args.serial_log)]
    if reacquire:
        command.append('--reacquire-once')
        if args.restore_pll:
            command.append('--restore-pll')
        if args.restore_gain:
            command.append('--restore-gain')
    print(f'Starting {name}: {samples} samples, single recovery={reacquire}', flush=True)
    RUN.main(command)
    # No retry based on exit code alone. Independently require the complete
    # capture, no kernel faults, safe session and successful input teardown.
    report = REPORT.summarize(capture, allow_downstream_failure=True)
    diagnostics = RECEIVER.summarize(capture)
    for filename, data in (('observations.json', report), ('receiver-report.json', diagnostics)):
        (capture / filename).write_text(json.dumps(data, indent=2) + '\n')
    return report


def execute(args):
    args.output.mkdir(mode=0o700)
    record = dict(schema_version=1, status='running', optical_service_verified=False,
                  artifact=str(args.artifact), restore_pll=args.restore_pll, restore_gain=args.restore_gain, started=time.time(), stages=[])
    path = args.output / 'matrix.json'
    def save():
        path.write_text(json.dumps(record, indent=2) + '\n')
    save()
    try:
        baseline = stage(args, 'baseline', 30, False)
        record['stages'].append(baseline)
        save()
        name, reacquire = followup(baseline)
        result = stage(args, name, args.extended_samples, reacquire)
        record['stages'].append(result)
        record.update(status='completed', downstream_stable=result['receive']['downstream_stable'])
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        record.update(status='stopped', error=str(error))
    finally:
        record['finished'] = time.time()
        save()
    print(json.dumps(record, indent=2))
    # Completed means evidence collection finished, not that PON works.
    return 0 if record['status'] == 'completed' and record['downstream_stable'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--inputs', required=True, type=Path)
    parser.add_argument('--fiber-connected', action='store_true', required=True)
    parser.add_argument('--serial-log', type=Path, default=Path('/tmp/serial_output.log'))
    parser.add_argument('--extended-samples', type=int, choices=(90, 180), default=90)
    parser.add_argument('--restore-pll', action='store_true',
                        help='Add PLL restoration to recovery only; baseline stays unchanged')
    parser.add_argument('--restore-gain', action='store_true',
                        help='Add OEM receiver gain to recovery only; baseline stays unchanged')
    parser.add_argument('--dry-run', action='store_true', help='Describe the matrix without SSH or writes')
    args = parser.parse_args()
    for name in ('artifact', 'output', 'inputs', 'serial_log'):
        setattr(args, name, getattr(args, name).resolve())
    if args.dry_run:
        print(json.dumps(dict(artifact=str(args.artifact), host=RUN.HOST,
                              baseline_samples=30, extended_samples=args.extended_samples,
                              followup='Observe longer if stable; otherwise one recovery if light stays present',
                              maximum_reacquisitions=1, restore_pll=args.restore_pll, restore_gain=args.restore_gain, optical_tx=False, flash=False), indent=2))
        return 0
    return execute(args)


if __name__ == '__main__':
    raise SystemExit(main())
