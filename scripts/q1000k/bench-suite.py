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
DEEP_PROBES=('oem-post-init','oem-post-cal') + RUN.PROBES[22:35]
CONNECTED_PROBES=DEEP_PROBES + tuple(name for name in RUN.PROBES[:22] if name != 'checker-dark')
CONTROLLER_CASES={'oem-md32':dict(oem_md32=True,rx_output='unchanged')}
for output in ('400-flat','600-flat','600-boost'):
    CONTROLLER_CASES['rx-output-'+output]=dict(oem_md32=False,rx_output=output)
    CONTROLLER_CASES['oem-acquire-'+output]=dict(oem_md32=False,rx_output=output)
    CONTROLLER_CASES['oem-md32-acquire-'+output]=dict(oem_md32=True,rx_output=output)
for output in ('400-flat','600-flat','600-boost'):
    CONTROLLER_CASES['oem-cal-'+output]=dict(oem_md32=False,rx_output=output)
    CONTROLLER_CASES['oem-post-cal-'+output]=dict(oem_md32=False,rx_output=output)
CASES=(('baseline',None,False),) + tuple(
    (name,name,True) for name in CONNECTED_PROBES) + (('public-recovery',None,True),) + tuple(
    (name, 'oem-post-cal' if name.startswith('oem-post-cal-') else
          'oem-cal-reset' if name.startswith('oem-cal-') else
          'oem-rx-acquire' if 'acquire' in name else None,
     'acquire' in name or '-cal-' in name)
    for name in CONTROLLER_CASES) + (('baseline-repeat',None,False),)
COVERAGE={
 'optical-power': dict(cases=['baseline','baseline-repeat'], shared='Controller RX nW/dBm every sample',
                      external='Compare same-fiber gateway reading; wavelength-selective calibrated meter settles absolute level.'),
 'false-light': dict(cases=['baseline','baseline-repeat'], shared='Controller LOS, raw PHY LOS/polarity and optical power together',
                    external='Dark and reconnected controls using this image; optional live disconnect/reconnect within a 180-s baseline tests IRQ response.'),
 'receiver-gain': dict(cases=['gain-auto','gain-low']+[n for n in CONNECTED_PROBES if n.startswith('oem-eye-') or n in ('eye-current','oem-analog','oem-cal-reset','oem-cal-auto')]+[name for name in CONNECTED_PROBES if name == 'oem-peaking'], shared='Gain, equalization, analog and controller readbacks',
                      external='OEM gain=1 and gain+PLL already failed on prior bench. Unknown equalizer/calibration values require board evidence, not a blind sweep.'),
 'clock-rate-reset': dict(cases=['oem-full-reset','oem-analog','oem-cal-reset','oem-cal-auto','public-recovery','tdc-delay','pll-order','oem-order']+[name for name in CONNECTED_PROBES if name in ('cdr-auto-release','cdr-internal-auto','prcal-finalize','fll-auto','rx-sequence-auto','post-eye-ready','oem-clock-cycle','oem-rx-acquire','combined-auto','prcal-rerun')],
                         shared='RX/PLL/PMA/TDC frequency monitors, targets, dividers, reset and sequence controls',
                         external='Independent recovered-clock measurement if readbacks remain ambiguous; The separate oem-full-reset case uses the exact OEM twelve-bit reset; upper-bit meanings remain unknown.'),
 'receive-path': dict(cases=['eye-current','oem-post-init','oem-post-cal','bit-order','checker']+[name for name in CONTROLLER_CASES if name.startswith('rx-output-')], shared='Input mux, bus width, data route, fixed EN7573 RX output settings and independent RX checker',
                      external='Establish EN7573-to-SoC RX differential continuity/polarity from schematic or identified test points. No documented polarity switch exists in imported source.'),
 'pcs-framing': dict(cases=['bit-order','descrambler','fec-oc','fec-off'], shared='PSync, HEC, codeword, frame and FEC counters',
                     external='All are receiver settings. No TX framing, optical activation or OLT configuration is changed.'),
 'measurement-boundary': dict(cases=['checker'], shared='Independent PMA checker alongside seven PCS boundaries and frame counter',
                             external='Normal optical traffic is not PRBS. A quiet checker is inconclusive; errors do not quantify BER.'),
 'line-type': dict(cases=['baseline','checker'], shared='Configured rate and frequency-window raw evidence',
                   external='Confirm gateway optical mode/module on the working line. Wavelength/modulation need appropriate external measurement if still uncertain.'),
 'analog-health': dict(cases=['oem-analog','oem-cal-reset','oem-cal-auto','oem-post-init','oem-post-cal','gain-auto','gain-low','oem-order','baseline-repeat']+list(CONTROLLER_CASES),
                      shared='Firmware/calibration hashes, controller MCU/status, optical power and analog readbacks',
                      external='Known-good optics/board or suitable electrical measurement separates hardware health from calibration. No EEPROM/calibration writes.'),
}


def plan(samples, selected_case=None):
    cases = [dict(name=n,probe=p,reacquire=r,samples=samples if r or n in CONTROLLER_CASES else 30,
                  **CONTROLLER_CASES.get(n,dict(oem_md32=False,rx_output='unchanged'))) for n,p,r in CASES]
    if selected_case is not None:
        cases = [case for case in cases if case['name'] == selected_case]
        if not cases:
            raise ValueError('Unknown or excluded case: ' + selected_case)
    return dict(schema_version=1,host=RUN.HOST,optical_tx=False,flash=False,
                cases=cases,automatic_retries=False,
                coverage=COVERAGE,external_tests_status='pending physical evidence; not automated',
                stop_conditions=['Any guard, kernel, diagnostic, cleanup or input-removal failure',
                                 'Light is lost during a connected case', 'Missing single probe attempt or restored-field marker'],
                collection_completion_is_not_optical_service_validation=True)


def check_artifact_compatibility(cases, version):
    """Reject unsupported modes locally, before staging anything on a device."""
    if type(version) is not int or version not in (1,2,3,4):
        raise ValueError('Unsupported artifact receiver diagnostics schema')
    if version == 3 and any(case.get("probe") in RUN.PROBES[22:] for case in cases):
        raise ValueError("Artifact diagnostics schema 4 is required for deep receiver cases")
    legacy_probes=RUN.PROBES[:10]
    unsupported=[case['name'] for case in cases if version < 3 and (
        case.get('probe') not in (None,*legacy_probes) or case.get('oem_md32',False) or
        case.get('rx_output','unchanged') != 'unchanged')]
    if unsupported:
        raise ValueError('Artifact diagnostics schema 3 is required for planned cases: '+', '.join(unsupported))


def stage(args,name,probe,reacquire):
    capture=args.output/name
    command=['receive','--artifact',str(args.artifact),'--output',str(capture),
             '--inputs',str(args.inputs),'--fiber-connected','--samples',str(args.samples if reacquire or name in CONTROLLER_CASES else 30),
             '--serial-log',str(args.serial_log)]
    if reacquire: command.append('--reacquire-once')
    if probe: command.extend(['--probe',probe])
    selection=CONTROLLER_CASES.get(name,{})
    if selection.get('oem_md32'): command.append('--oem-md32')
    if selection.get('rx_output','unchanged') != 'unchanged':
        command.extend(['--rx-output',selection['rx_output']])
    print(f'Starting {name}; optical TX remains inhibited.',flush=True)
    RUN.main(command)
    report=REPORT.summarize(capture,allow_downstream_failure=True)
    receiver=RECEIVER.summarize(capture)
    check_controller_selection(capture,selection)
    if not report.get('probe_diagnostics'):
        raise ValueError('This suite requires the new diagnostic image')
    for filename,data in [('observations.json',report),('receiver-report.json',receiver),
                          ('hypotheses.json',HYPOTHESES.evaluate(receiver)),
                          ('probe-report.json',report['probe_diagnostics'])]:
        (capture/filename).write_text(json.dumps(data,indent=2)+'\n')
    return report


def check_controller_selection(capture,case):
    checkpoint=json.loads((capture/'checkpoint.json').read_text())
    if (type(checkpoint.get('oem_md32',False)) is not bool or
            checkpoint.get('oem_md32',False) != case.get('oem_md32',False) or
            checkpoint.get('rx_output','unchanged') != case.get('rx_output','unchanged')):
        raise ValueError('Controller experiment selection did not match the capture')


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
    record=plan(args.samples,getattr(args,'case',None))
    check_artifact_compatibility(record['cases'],RUN.diagnostics_version(args.artifact))
    args.output.mkdir(mode=0o700)
    record.update(status='running',artifact=str(args.artifact),started=time.time(),results=[])
    path=args.output/'suite.json'
    def save(): path.write_text(json.dumps(record,indent=2)+'\n')
    save()
    try:
        for case in record['cases']:
            name,probe,reacquire=case['name'],case['probe'],case['reacquire']
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
        record['not_run']=[case['name'] for case in record['cases'] if case['name'] not in {r['name'] for r in record['results']}]
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
    parser.add_argument('--case',choices=tuple(case[0] for case in CASES),help='One named case only; no automatic retry')
    parser.add_argument('--dry-run',action='store_true')
    args=parser.parse_args()
    for field in ('artifact','output','inputs','serial_log'):
        setattr(args,field,getattr(args,field).resolve())
    if args.dry_run:
        record=plan(args.samples,args.case)
        check_artifact_compatibility(record['cases'],RUN.diagnostics_version(args.artifact))
        print(json.dumps(record,indent=2))
        return 0
    return execute(args)

if __name__=='__main__':
    raise SystemExit(main())
