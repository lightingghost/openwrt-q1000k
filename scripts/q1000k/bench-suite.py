#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""All remaining RX hypotheses on one TX-inhibited RAM image.

Each case shares concurrent diagnostics. Register experiments are sequential,
with full normal unload/controller-off/input cleanup between cases. External
physical controls are listed explicitly and cannot be fabricated by software.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import time


def module(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),Path(__file__).with_name(name+'.py'))
    result=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result

RUN=module('bench-run')
REPORT=module('bench-report')
RECEIVER=module('bench-receiver-report')
HYPOTHESES=module('bench-hypotheses')

# Cases map onto the immutable module enum; no arbitrary addresses or values.
CASES=(('baseline',None,False), ('public-recovery',None,True)) + tuple(
    (name,name,True) for name in RUN.PROBES) + (('baseline-repeat',None,False),)
COVERAGE={
 'optical-power': dict(cases=['baseline','baseline-repeat'], shared='Controller RX nW/dBm every sample',
                      external='Compare same-fiber gateway reading; wavelength-selective calibrated meter settles absolute level.'),
 'false-light': dict(cases=['baseline','baseline-repeat'], shared='Controller LOS, raw PHY LOS/polarity and optical power together',
                    external='Dark and reconnected controls using this image; optional live disconnect/reconnect within a 180-s baseline tests IRQ response.'),
 'receiver-gain': dict(cases=['gain-auto','gain-low'], shared='Gain, equalization, analog and controller readbacks',
                      external='OEM gain=1 and gain+PLL already failed on prior bench. Unknown equalizer/calibration values require board evidence, not a blind sweep.'),
 'clock-rate-reset': dict(cases=['public-recovery','tdc-delay','pll-order','oem-order'],
                         shared='RX/PLL/PMA/TDC frequency monitors, targets, dividers, reset and sequence controls',
                         external='Independent recovered-clock measurement if readbacks remain ambiguous; unknown upper reset bits are excluded.'),
 'receive-path': dict(cases=['bit-order','checker'], shared='Input mux, bus width, data route and independent RX checker',
                      external='Establish EN7573-to-SoC RX differential continuity/polarity from schematic or identified test points. No documented polarity switch exists in imported source.'),
 'pcs-framing': dict(cases=['bit-order','descrambler','fec-oc','fec-off'], shared='PSync, HEC, codeword, frame and FEC counters',
                     external='All are receiver settings. No TX framing, optical activation or OLT configuration is changed.'),
 'measurement-boundary': dict(cases=['checker'], shared='Independent PMA checker alongside seven PCS boundaries and frame counter',
                             external='Normal optical traffic is not PRBS. A quiet checker is inconclusive; errors do not quantify BER.'),
 'line-type': dict(cases=['baseline','checker'], shared='Configured rate and frequency-window raw evidence',
                   external='Confirm gateway optical mode/module on the working line. Wavelength/modulation need appropriate external measurement if still uncertain.'),
 'analog-health': dict(cases=['gain-auto','gain-low','oem-order','baseline-repeat'],
                      shared='Firmware/calibration hashes, controller MCU/status, optical power and analog readbacks',
                      external='Known-good optics/board or suitable electrical measurement separates hardware health from calibration. No EEPROM/calibration writes.'),
}


def plan(samples):
    return dict(schema_version=1,host=RUN.HOST,optical_tx=False,flash=False,
                cases=[dict(name=n,probe=p,reacquire=r,samples=30 if not r else samples) for n,p,r in CASES],
                coverage=COVERAGE,external_tests_status='pending physical evidence; not automated',
                stop_conditions=['Any guard, kernel, diagnostic, cleanup or input-removal failure',
                                 'Light is lost during a connected case', 'Missing single probe attempt or restored-field marker'],
                collection_completion_is_not_optical_service_validation=True)


def stage(args,name,probe,reacquire):
    capture=args.output/name
    command=['receive','--artifact',str(args.artifact),'--output',str(capture),
             '--inputs',str(args.inputs),'--fiber-connected','--samples',str(args.samples if reacquire else 30),
             '--serial-log',str(args.serial_log)]
    if reacquire: command.append('--reacquire-once')
    if probe: command.extend(['--probe',probe])
    print(f'Starting {name}; optical TX remains inhibited.',flush=True)
    RUN.main(command)
    report=REPORT.summarize(capture,allow_downstream_failure=True)
    receiver=RECEIVER.summarize(capture)
    if not report.get('probe_diagnostics'):
        raise ValueError('This suite requires the new diagnostic image')
    for filename,data in [('observations.json',report),('receiver-report.json',receiver),
                          ('hypotheses.json',HYPOTHESES.evaluate(receiver)),
                          ('probe-report.json',report['probe_diagnostics'])]:
        (capture/filename).write_text(json.dumps(data,indent=2)+'\n')
    return report


def check_case(report,probe,reacquire):
    rx=report['receive']; diag=report['probe_diagnostics']
    if report['cleanup']!='passed' or report['fiber']!='connected':
        raise ValueError('Case did not finish with verified cleanup')
    if rx['controller_los']!=[False] or rx['phy_los']!=[False]:
        raise ValueError('Light was not continuously present; stop to inspect the connection')
    if diag['probe']!=probe or rx['reacquire_requested']!=reacquire:
        raise ValueError('Case selection did not match the captured evidence')
    if reacquire and not rx['downstream_stable'] and diag['attempts']!=1:
        raise ValueError('Unsuccessful receive case did not execute its one-attempt experiment')
    # A already-synchronized receiver does not satisfy the no-sync trigger.
    return 'observed' if not reacquire or diag['attempts'] else 'not-triggered-already-synchronized'


def execute(args):
    args.output.mkdir(mode=0o700)
    record=plan(args.samples)
    record.update(status='running',artifact=str(args.artifact),started=time.time(),results=[])
    path=args.output/'suite.json'
    def save(): path.write_text(json.dumps(record,indent=2)+'\n')
    save()
    try:
        for name,probe,reacquire in CASES:
            result=stage(args,name,probe,reacquire)
            status=check_case(result,probe,reacquire)
            record['results'].append(dict(name=name,status=status,
                downstream_stable=result['receive']['downstream_stable'],report=result))
            save()
        record['status']='collection-complete'
    except (OSError,RuntimeError,ValueError,KeyError,TypeError) as error:
        record.update(status='stopped',error=str(error))
    finally:
        record['finished']=time.time()
        record['not_run']=[n for n,_,_ in CASES if n not in {r['name'] for r in record['results']}]
        save()
    print(json.dumps(dict(status=record['status'],record=str(path),not_run=record['not_run']),indent=2))
    return 0 if record['status']=='collection-complete' else 1


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--inputs',type=Path,required=True)
    parser.add_argument('--fiber-connected',action='store_true',required=True)
    parser.add_argument('--serial-log',type=Path,default=Path('/tmp/serial_output.log'))
    parser.add_argument('--samples',type=int,choices=(30,90,180),default=90)
    parser.add_argument('--dry-run',action='store_true')
    args=parser.parse_args()
    for field in ('artifact','output','inputs','serial_log'):
        setattr(args,field,getattr(args,field).resolve())
    if args.dry_run:
        print(json.dumps(plan(args.samples),indent=2))
        return 0
    return execute(args)

if __name__=='__main__':
    raise SystemExit(main())
