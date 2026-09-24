#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build or run one portable Q1000K RX evidence collector (Python 3 + OpenSSH).

A generated copy pins its RAM image and contains the exact existing guard and
report implementations. It never flashes, boots, or enables optical TX.
"""
import argparse
import base64
from contextlib import contextmanager, ExitStack
import fcntl
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import zlib

EMBEDDED = None
HOST = '192.168.0.1'
IMAGE = 'openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb'
COMPANIONS = ('bench-run', 'bench-report', 'bench-receiver-report', 'bench-probe-report',
              'bench-hypotheses', 'bench-suite', 'bench-control-report', 'bench-serial')
HEADER = 'package/kernel/airoha-pon/src/bsp/include/q1000k_rx_diag.h'
ARTIFACT_FILES = ('selection.json', 'runtime-sha256sums', 'checkpoint.json',
                  'runtime/usr/sbin/q1000k-pon-bench')
CASE_FILES = ('checkpoint.json', 'baseline.log', 'stage.log', 'attempt.log', 'postflight.log',
              'cleanup.log', 'serial.log', 'observations.json', 'receiver-report.json',
              'probe-report.json', 'hypotheses.json', 'control-report.json', 'operator-events.json')
MIN_PHASE_SAMPLES = 15


def module(root, name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), root/'scripts/q1000k'/f'{name}.py')
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')
    path.chmod(0o600)


def artifact_metadata(artifact, run):
    selection = json.loads((artifact/'selection.json').read_text())
    checkpoint = json.loads((artifact/'checkpoint.json').read_text())
    revision = selection.get('revision', '')
    if not re.fullmatch('[0-9a-f]{40}', revision):
        raise ValueError('Invalid artifact source revision')
    if checkpoint.get('status') != 'passed' or checkpoint.get('revision') != revision:
        raise ValueError('Artifact must have a passed, source-matched build checkpoint')
    if not re.fullmatch('[0-9a-f]{64}', checkpoint.get('sha256', '')):
        raise ValueError('Build checkpoint lacks its image checksum')
    run.runtime_manifest(artifact/'runtime-sha256sums')
    helper = (artifact/'runtime/usr/sbin/q1000k-pon-bench').read_bytes()
    if f'{sha256(helper)}  /usr/sbin/q1000k-pon-bench' not in (artifact/'runtime-sha256sums').read_text().splitlines():
        raise ValueError('Artifact helper differs from its runtime checksum')
    return dict(revision=revision, image=IMAGE, image_sha256=checkpoint['sha256'],
                diagnostics_version=run.diagnostics_version(artifact),
                runtime_manifest_sha256=sha256((artifact/'runtime-sha256sums').read_bytes()))


def build_single_file(output, artifact):
    if EMBEDDED is not None:
        raise ValueError('Generate collectors from the source checkout')
    root = Path(__file__).resolve().parents[2]
    run = module(root, 'bench-run')
    metadata = artifact_metadata(artifact, run)
    if sha256((artifact/IMAGE).read_bytes()) != metadata['image_sha256']:
        raise ValueError('Firmware image differs from its passed checkpoint')
    files = {f'scripts/q1000k/{name}.py': (root/'scripts/q1000k'/f'{name}.py').read_text()
             for name in COMPANIONS}
    files[HEADER] = (root/HEADER).read_text()
    files.update({f'artifact/{name}': (artifact/name).read_text() for name in ARTIFACT_FILES})
    bundle = dict(schema_version=1, files=files,
                  hashes={name: sha256(data.encode()) for name, data in files.items()})
    encoded = base64.b64encode(zlib.compress(json.dumps(bundle).encode(), 9)).decode()
    source = Path(__file__).read_text()
    marker = 'EMBEDDED = ' + 'None\n'
    if source.count(marker) != 1:
        raise ValueError('Collector embedding marker is ambiguous')
    with output.open('x') as stream:
        stream.write(source.replace(marker, f'EMBEDDED = {encoded!r}\n'))
    output.chmod(0o700)
    print(json.dumps(dict(collector=str(output), sha256=sha256(output.read_bytes()), **metadata), indent=2))
    return 0


@contextmanager
def workspace(artifact=None):
    if EMBEDDED is None:
        if artifact is None:
            raise ValueError('Source collector requires --artifact; generated copies do not')
        yield Path(__file__).resolve().parents[2], artifact
        return
    if artifact is not None:
        raise ValueError('Generated collector uses its pinned artifact; --artifact is not allowed')
    bundle = json.loads(zlib.decompress(base64.b64decode(EMBEDDED)))
    expected = {f'scripts/q1000k/{n}.py' for n in COMPANIONS} | {HEADER}
    expected |= {f'artifact/{n}' for n in ARTIFACT_FILES}
    if bundle.get('schema_version') != 1 or set(bundle['files']) != expected or set(bundle['hashes']) != expected:
        raise ValueError('Unexpected embedded collector files')
    with tempfile.TemporaryDirectory(prefix='q1000k-collector-') as directory:
        root = Path(directory)
        for name, data in bundle['files'].items():
            if sha256(data.encode()) != bundle['hashes'][name]:
                raise ValueError('Embedded file checksum mismatch: ' + name)
            path = root/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(data)
        (root/'tmp').mkdir()
        yield root, root/'artifact'


@contextmanager
def device_lock():
    # Stable across independent generated copies and their temporary workspaces.
    path = Path(tempfile.gettempdir())/f'q1000k-bench-{HOST}.lock'
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


class Stop:
    def __init__(self):
        self.reason = None

    def request(self, reason):
        if self.reason is None:
            self.reason = reason
            print('\nStopping after the active bounded capture and normal cleanup. Keep the bench powered.', flush=True)

    def signal(self, signum, frame):
        self.request('Operator interrupted collection')


@contextmanager
def interrupts(stop):
    previous = {number: signal.signal(number, stop.signal) for number in (signal.SIGINT, signal.SIGTERM)}
    try:
        yield
    finally:
        for number, handler in previous.items():
            signal.signal(number, handler)


def plan(baseline_only=False, suite=None, skip_live_control=False, selected_case=None, samples=90):
    """Pin every immutable experiment in the plan before any device access."""
    if suite is None:
        suite = module(Path(__file__).resolve().parents[2], 'bench-suite')
    cases = [dict(name='connected-baseline', samples=30, fiber='connected',
                  probe=None, reacquire=False)]
    if not baseline_only:
        cases.extend(dict(case, fiber='connected') for case in suite.plan(samples)['cases']
                     if case['name'] != 'baseline')
        if not skip_live_control:
            if 'checker-dark' in suite.RUN.PROBES:
                cases.append(dict(name='checker-dark', samples=180, fiber='connected-dark-reconnected',
                                  probe='checker-dark', reacquire=True,
                                  minimum_samples_per_phase=MIN_PHASE_SAMPLES))
            cases.append(dict(name='live-reconnect', samples=180, fiber='connected-dark-reconnected',
                              probe=None, reacquire=False, minimum_samples_per_phase=MIN_PHASE_SAMPLES))
    if selected_case is not None:
        cases = [case for case in cases if case['name'] == selected_case]
        if not cases:
            raise ValueError('Unknown or excluded case: ' + selected_case)
    coverage = {name: dict(value, cases=['connected-baseline' if case == 'baseline' else case
                                       for case in value['cases']])
                for name, value in suite.COVERAGE.items()}
    if any(case['name'] == 'checker-dark' for case in cases):
        coverage['measurement-boundary']['cases'].append('checker-dark')
    return dict(schema_version=2, host=HOST,
                mode='baseline-only' if baseline_only else 'experiments', cases=cases,
                optical_tx=False, flash=False,
                register_probe='immutable per-case selection' if any(case.get('probe') for case in cases) else None,
                coverage=coverage, automatic_retries=False,
                bounded_recovery=dict(suite.plan(samples)['bounded_recovery'], zero_attempt_control='connected-baseline'),
                minimum_sampling_seconds=sum(case['samples'] for case in cases),
                optical_service_verified=False, signal_quality_verified=False,
                live_reconnection_tested=False, collection_completion_is_not_optical_service_validation=True,
                remaining_hypotheses={
                    'clock-reset': dict(software='Compare zero, one and up to six identical OEM reset/clock acquisitions; retain per-attempt timing, counters, NCPO, frequency and FIFO status',
                                        pending='Actual recovered-clock/data measurement and documented reset interpretation'),
                    'route-polarity': dict(software='Exercise fixed RX electrical-output settings, retain input-route and PCS counter evidence',
                                           pending='Electrical continuity/polarity measurement or board-specific OEM comparison'),
                    'analog-calibration': dict(software='Recalibrate at OEM gain/peaking, take a fresh bounded eye measurement, and test the OEM controller post-init bit',
                                              pending='Analog signal quality and calibration accuracy require external evidence'),
                    'pcs-packing': dict(software='Correlate seven PCS counters; previous isolated bit-order/FEC/descrambler trials failed',
                                       pending='Earlier receive-path failure or undocumented combined configuration remains possible'),
                    'live-insertion': dict(software='Capture confirmed connected/dark/reconnected phases without unloading',
                                          pending='This collection tests sampled response; exact physical latency is not measured'),
                    'external-line-wavelength': dict(software='Record average received power and LOS',
                                                    pending='Same-line gateway comparison and wavelength-selective measurement'),
                    'undocumented-oem-resets': dict(software='Compare known-seven-bit and exact-OEM-twelve-bit reset after static verification in both firmware versions',
                                                   pending='Exact twelve-bit OEM reset is tested separately; meanings of upper five bits remain undocumented'),
                },
                limits=['Power and LOS do not prove wavelength, modulation quality or recovered-clock lock.',
                        'Operator confirmation time is not the precise time of the physical transition.',
                        'A negative experiment excludes only its exact recipe, not the entire hypothesis.',
                        'Every connected experiment unloads the stack and removes private RAM inputs before the next case.',
                        'The 180-sample live window is bounded; missing confirmation or phase stops collection without retry.',
                        'External optical/electrical measurements and documented OEM configuration evidence remain required.'])


def read_answer():
    """Nonblocking line input so a completed capture never waits for a prompt."""
    if not select.select([sys.stdin], [], [], 0)[0]:
        return None
    value = sys.stdin.readline()
    if value == '':
        raise EOFError('Operator input closed')
    return value.strip()


def confirm(message, word, stop, events, serial_capture=None):
    print(f'{message}\nType {word} to confirm, or STOP to end: ', end='', flush=True)
    while not stop.reason:
        if serial_capture is not None:
            serial_capture.check()
        answer = read_answer()
        if answer is None:
            time.sleep(0.1)
            continue
        if answer == word:
            events.append(dict(action=word, confirmed_at=time.time(), kind='before-start'))
            return True
        if answer.upper() in ('STOP', 'Q', 'QUIT'):
            stop.request('Operator cancelled before the next capture')
            return False
        print(f'Type exactly {word}, or STOP: ', end='', flush=True)
    return False


def rx_rows(path):
    if not path.exists():
        return []
    text = path.read_text(errors='replace')
    rows = []
    # SSH can be writing the final line while it is read.
    for line in text.splitlines(keepends=True):
        if not line.endswith('\n') or not line.startswith('{'):
            continue
        row = json.loads(line)
        if 'rx_bench' in row:
            rows.append(row)
    return rows


def consecutive_phase(rows, start, los, count=MIN_PHASE_SAMPLES, attempts=None):
    streak = []
    for number, row in enumerate(rows[start:], start + 1):
        if (row.get('controller_los') is los and row.get('phy_los') is los and
                (attempts is None or row.get('reacquire_attempts') == attempts)):
            streak.append((number, row))
            if len(streak) == count:
                first_number, first = streak[0]
                return dict(first_sample=first_number, last_sample=number,
                            first_sampled_ms=first['sampled_ms'], last_sampled_ms=row['sampled_ms'],
                            first_irq_calls=first['irq_calls'], last_irq_calls=row['irq_calls'],
                            first_poll_calls=first['poll_calls'], last_poll_calls=row['poll_calls'],
                            controller_los=los, phy_los=los)
        else:
            streak = []
    return None


class LiveControl:
    """Prompts advance only after confirmed and observed ordered physical states."""
    def __init__(self, capture, stop):
        self.capture, self.stop = capture, stop
        self.state, self.start = 'lit', 0
        self.events, self.phases = [], []
        self.prompt_start = None

    def save(self):
        if self.capture.exists():
            write_json(self.capture/'operator-events.json', self.result())

    def result(self):
        return dict(status='complete' if self.state == 'complete' else 'incomplete',
                    events=self.events, observed_phases=self.phases,
                    limits=['Confirmation timestamps are operator reports, not precise physical transition times.',
                            'Sampled detection and IRQ/poll counts are evidence; they are not calibrated interrupt latency.'])

    def phase(self, rows):
        return consecutive_phase(rows, self.start, self.state == 'dark')

    def advance(self, rows, allow_prompts=True):
        if self.stop.reason or self.state == 'complete':
            return
        if self.state.startswith('confirm-'):
            if not allow_prompts:
                return
            answer = read_answer()
            if answer is None:
                return
            word = 'DISCONNECTED' if self.state == 'confirm-dark' else 'CONNECTED'
            if answer.upper() in ('STOP', 'Q', 'QUIT'):
                self.stop.request('Operator cancelled live physical control')
                return
            if answer != word:
                print(f'Type exactly {word}, or STOP: ', end='', flush=True)
                return
            self.events.append(dict(action=word, confirmed_at=time.time(), samples_seen=len(rows),
                                    after_prompt_sample=self.prompt_start))
            self.start = self.prompt_start
            self.state = 'dark' if word == 'DISCONNECTED' else 'reconnected'
            self.save()
            return
        phase = self.phase(rows)
        if phase is None:
            return
        if not allow_prompts and self.state != 'reconnected':
            return
        phase['name'] = self.state
        if self.phases:
            previous = rows[self.phases[-1]['last_sample'] - 1]
            first = rows[phase['first_sample'] - 1]
            phase['irq_change_since_previous_phase_end'] = first['irq_calls'] - previous['irq_calls']
            phase['poll_change_since_previous_phase_end'] = first['poll_calls'] - previous['poll_calls']
        self.phases.append(phase)
        if self.state == 'reconnected':
            self.state = 'complete'
            print('\nReconnected light observed. Leave the fiber connected while the capture finishes and cleans up.', flush=True)
        else:
            self.prompt_start = len(rows)
            if self.state == 'lit':
                self.state = 'confirm-dark'
                message, word = 'Now disconnect the fiber from this bench.', 'DISCONNECTED'
            else:
                self.state = 'confirm-reconnected'
                message, word = 'Now reconnect the fiber to this bench.', 'CONNECTED'
            self.events.append(dict(action='prompt-' + word, prompted_at=time.time(), samples_seen=len(rows)))
            print(f'\n{message}\nType {word} after doing so, or STOP: ', end='', flush=True)
        self.save()


class FreshCheckerControl(LiveControl):
    """Arm the immutable checker once in darkness after illuminated startup."""
    def phase(self, rows):
        return consecutive_phase(rows, self.start, self.state == 'dark',
                                 attempts=0 if self.state == 'lit' else 1)

    def result(self):
        return dict(super().result(), probe='checker-dark',
                    limits=super().result()['limits'] + [
                        'The checker must arm once in darkness before reconnection is requested.',
                        'Normal traffic is not PRBS; checker activity is not a BER measurement.',
                        'Compare against the separate illuminated checker case; constant status may be latched.'])


def capture_case(args, root, artifact, case, stop):
    if args.serial_capture is not None:
        args.serial_capture.check()
    capture = args.output/case['name']
    command = [sys.executable, str(root/'scripts/q1000k/bench-run.py'), 'receive',
               '--artifact', str(artifact), '--output', str(capture), '--inputs', str(args.inputs),
               '--fiber-connected', '--samples', str(case['samples']), '--serial-log', str(args.serial_log)]
    if case.get('reacquire'):
        command.append('--reacquire-once')
    if case.get('probe'):
        command.extend(['--probe', case['probe']])
    if case.get('oem_md32'):
        command.append('--oem-md32')
    if case.get('rx_output', 'unchanged') != 'unchanged':
        command.extend(['--rx-output', case['rx_output']])
    live = (FreshCheckerControl(capture, stop) if case['name'] == 'checker-dark' else
            LiveControl(capture, stop) if case['name'] == 'live-reconnect' else None)
    print(f"Starting {case['name']} ({case['samples']} samples); optical TX remains inhibited.", flush=True)
    with (args.output/(case['name']+'-runner.log')).open('wb') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            while process.poll() is None:
                if args.serial_capture is not None and not stop.reason:
                    try:
                        args.serial_capture.check()
                    except (OSError, RuntimeError) as error:
                        stop.request('Serial capture failed: '+str(error))
                if live and not stop.reason:
                    try:
                        observed_rows = rx_rows(capture/'attempt.log')
                        live.advance(observed_rows, allow_prompts=len(observed_rows) < case['samples'])
                    except (EOFError, OSError, ValueError, KeyError, TypeError) as error:
                        stop.request(str(error))
                time.sleep(0.2)
            if live and not stop.reason:
                # Observe the final batch without accepting input after capture completion.
                if not live.state.startswith('confirm-'):
                    live.advance(rx_rows(capture/'attempt.log'), allow_prompts=False)
        finally:
            # Never kill SSH while it may own modules or private RAM files. The
            # exact runner bounds every command and performs guarded teardown.
            while process.poll() is None:
                time.sleep(0.2)
            if live:
                live.save()
    return live.result() if live else None


def summarize_case(root, capture, live=None, case=None):
    if not (capture/'serial.log').read_bytes():
        raise ValueError('Serial evidence is empty; an active serial logger/device is required')
    control = module(root, 'bench-control-report')
    report, observations, receiver = control.summarize(capture)
    control.SUITE.check_controller_selection(capture, case or {})
    for name, data in (('observations.json', observations), ('receiver-report.json', receiver),
                       ('probe-report.json', observations['probe_diagnostics']),
                       ('hypotheses.json', control.SUITE.HYPOTHESES.evaluate(receiver)),
                       ('control-report.json', report)):
        write_json(capture/name, data)
    if live is None:
        status = control.SUITE.check_case(observations, (case or {}).get('probe'),
                                         (case or {}).get('reacquire', False))
    else:
        dark_checker = (case or {}).get('probe') == 'checker-dark'
        expected_probe = 'checker-dark' if dark_checker else None
        if (observations['probe_diagnostics']['probe'] != expected_probe or
                observations['receive']['reacquire_requested'] != dark_checker):
            raise ValueError('Live control experiment selection did not match the capture')
        if dark_checker and observations['probe_diagnostics']['attempts'] != 1:
            raise ValueError('Fresh dark checker did not execute its single attempt')
        if live['status'] != 'complete' or [p['name'] for p in live['observed_phases']] != ['lit', 'dark', 'reconnected']:
            raise ValueError('Live window lacks confirmed and observed connected/dark/reconnected phases')
        # Independently verify the ordered phase evidence against the complete,
        # safety-checked capture, not just the interactive monitoring state.
        rows = rx_rows(capture/'attempt.log')
        if dark_checker:
            first_attempt = next((index for index,row in enumerate(rows)
                                  if row.get('reacquire_attempts') == 1), None)
            if (first_attempt is None or
                    rows[first_attempt].get('controller_los') is not True or
                    rows[first_attempt].get('phy_los') is not True or
                    not any(row.get('controller_los') is False and row.get('phy_los') is False
                            for row in rows[:first_attempt])):
                raise ValueError('Fresh checker arm was not observed in darkness after illuminated startup')
        after = 0
        for index, los in enumerate((False, True, False)):
            phase = consecutive_phase(rows, after, los,
                                      attempts=(0 if index == 0 else 1) if dark_checker else None)
            if phase is None:
                raise ValueError('Complete capture does not contain all three ordered LOS phases')
            after = phase['last_sample']
        status = 'observed'
    return dict(status=status, cleanup='passed', downstream_stable=observations['receive']['downstream_stable'],
                probe=observations['probe_diagnostics']['probe'],
                attempts=observations['probe_diagnostics']['attempts'],
                optical_service_verified=False, signal_quality_verified=False)


def package_capture(output, record):
    """An explicit evidence allowlist never archives inputs or a whole directory."""
    target = output.parent/(output.name+'.tar.gz')
    names = ['collection.json', 'selection.json', 'runtime-sha256sums', 'build-checkpoint.json']
    if record.get('serial_device'):
        names.append('serial-source.log')
    for case in record['cases']:
        name = case['name']
        names.append(name+'-runner.log')
        names.extend(name+'/'+filename for filename in CASE_FILES)
    with target.open('xb') as destination:
        os.chmod(target, 0o600)
        with tarfile.open(fileobj=destination, mode='w:gz') as archive:
            for name in names:
                path = output/name
                if not path.exists():
                    continue
                if path.is_symlink() or not path.is_file() or (path.parent != output and path.parent.is_symlink()):
                    raise ValueError('Evidence is not a regular file: ' + name)
                data = path.read_bytes()
                member = tarfile.TarInfo(output.name+'/'+name)
                member.size, member.mode, member.mtime = len(data), 0o600, int(time.time())
                archive.addfile(member, io.BytesIO(data))
    return target


def execute(args, root, artifact, metadata):
    run = module(root, 'bench-run')
    suite = module(root, 'bench-suite')
    record = plan(args.baseline_only, suite,
                  getattr(args, 'skip_live_control', False), getattr(args, 'case', None),
                  getattr(args, 'samples', 90))
    suite.check_artifact_compatibility(record['cases'], metadata['diagnostics_version'])
    run.validate_inputs(args.inputs)
    # Serial evidence is necessary for the existing kernel-warning/restore checks.
    if args.serial_device is None:
        with args.serial_log.open('rb'):
            pass
    if not re.fullmatch('[-A-Za-z0-9_.]+', args.output.name):
        raise ValueError('Use a simple new output directory name')
    if (args.output.parent/(args.output.name+'.tar.gz')).exists():
        raise ValueError('Output evidence archive already exists')
    args.output.mkdir(mode=0o700)
    for original, name in (('selection.json', 'selection.json'), ('runtime-sha256sums', 'runtime-sha256sums'),
                           ('checkpoint.json', 'build-checkpoint.json')):
        (args.output/name).write_bytes((artifact/original).read_bytes())
    if args.serial_device is not None:
        args.serial_log = args.output/'serial-source.log'
    record.update(artifact=metadata, started=time.time(), status='running', results=[], confirmations=[],
                  serial_log=str(args.serial_log), serial_capture_required=True,
                  serial_device=str(args.serial_device) if args.serial_device else None,
                  archive=str(args.output.parent/(args.output.name+'.tar.gz')))
    stop = Stop()
    def save():
        record['not_run'] = [case['name'] for case in record['cases']
                             if case['name'] not in {r['name'] for r in record['results']}]
        write_json(args.output/'collection.json', record)
    save()
    args.serial_capture = None
    with interrupts(stop), ExitStack() as resources:
        try:
            if args.serial_device is not None:
                serial = module(root, 'bench-serial')
                args.serial_capture = resources.enter_context(serial.SerialCapture(args.serial_device, args.serial_log))
            connected_confirmed = False
            for case in record['cases']:
                if stop.reason:
                    break
                if args.fiber_connected:
                    record['confirmations'].append(dict(action='CONNECTED', kind='command-line', confirmed_at=time.time()))
                elif not connected_confirmed and not confirm('Connect the fiber and leave it connected throughout the acquisition experiments.',
                                 'CONNECTED', stop, record['confirmations'], args.serial_capture):
                    break
                connected_confirmed = True
                result = dict(name=case['name'], status='running', started=time.time())
                record['results'].append(result)
                save()
                try:
                    live = capture_case(args, root, artifact, case, stop)
                    if live:
                        result['physical_control'] = live
                    result.update(summarize_case(root, args.output/case['name'], live, case))
                    if live:
                        record['live_reconnection_tested'] = True
                    if stop.reason:
                        result['status'] = 'interrupted'
                except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
                    result.update(status='failed', error=str(error))
                    stop.request(str(error))
                result['finished'] = time.time()
                save()
            record['status'] = 'stopped' if stop.reason else 'collection-complete'
            if stop.reason:
                record['error'] = stop.reason
        except (EOFError, OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
            record.update(status='stopped', error=str(error))
        finally:
            try:
                resources.close()
            except (OSError, RuntimeError) as error:
                record.update(status='stopped', error='Serial capture failed: '+str(error))
            record['finished'] = time.time()
            save()
            package_capture(args.output, record)
    print(json.dumps(dict(status=record['status'], archive=record['archive'], not_run=record['not_run'],
                          optical_service_verified=False), indent=2))
    return 0 if record['status'] == 'collection-complete' else 1


def summarize_existing(root, output):
    record = json.loads((output/'collection.json').read_text())
    results = []
    for case in record['cases']:
        capture = output/case['name']
        if not capture.exists():
            results.append(dict(name=case['name'], status='not-run'))
            continue
        try:
            live = (json.loads((capture/'operator-events.json').read_text())
                    if case['name'] in ('live-reconnect', 'checker-dark') else None)
            results.append(dict(name=case['name'], **summarize_case(root, capture, live, case)))
        except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
            results.append(dict(name=case['name'], status='incomplete-or-invalid', error=str(error)))
    print(json.dumps(dict(optical_service_verified=False, results=results), indent=2))
    return 0 if all(result['status'] in ('observed', 'not-triggered-already-synchronized') for result in results) else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-single-file', type=Path, metavar='OUTPUT', help='Generate a portable collector from a passed artifact')
    parser.add_argument('--artifact', type=Path, help='Build artifact (source checkout only)')
    parser.add_argument('--inputs', type=Path, help='Existing private input tar; never included in evidence archive')
    parser.add_argument('--output', type=Path, help='New capture directory; also writes adjacent .tar.gz')
    serial = parser.add_mutually_exclusive_group()
    serial.add_argument('--serial-log', type=Path, default=Path('/tmp/serial_output.log'), help='Actively written serial capture (default: /tmp/serial_output.log)')
    serial.add_argument('--serial-device', type=Path, help='Capture a serial device at 115200 baud during the tests')
    parser.add_argument('--baseline-only', action='store_true', help='Only the 30-sample connected baseline; no interactive physical control')
    parser.add_argument('--experiments', action='store_true', help='Run every immutable acquisition experiment (the default)')
    parser.add_argument('--skip-live-control', action='store_true', help='Run connected experiments without prompted dark-checker/reconnect controls')
    parser.add_argument('--case', help='Run just this named case from --dry-run, without retrying any case')
    parser.add_argument('--samples', type=int, choices=(30, 90, 180), default=90, help='Samples per acquisition experiment (default: 90)')
    parser.add_argument('--fiber-connected', action='store_true', help='Confirm the fiber is connected before the first case')
    parser.add_argument('--dry-run', action='store_true', help='Show pinned image and plan without contacting the device')
    parser.add_argument('--summarize', type=Path, metavar='CAPTURE', help='Recheck existing evidence without device access')
    args = parser.parse_args(argv)
    for name in ('build_single_file', 'artifact', 'inputs', 'output', 'serial_log', 'serial_device', 'summarize'):
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, value.resolve())
    if args.build_single_file:
        if args.artifact is None:
            parser.error('--build-single-file requires --artifact')
        return build_single_file(args.build_single_file, args.artifact)
    if args.baseline_only and not args.fiber_connected and not args.dry_run and not args.summarize:
        parser.error('--baseline-only requires --fiber-connected as explicit physical confirmation')
    if args.baseline_only and (args.experiments or args.skip_live_control or args.case):
        parser.error('--baseline-only cannot be combined with experiment selection')
    with workspace(args.artifact) as (root, artifact):
        metadata = artifact_metadata(artifact, module(root, 'bench-run'))
        if args.dry_run:
            suite = module(root, 'bench-suite')
            record = plan(args.baseline_only, suite, args.skip_live_control, args.case, args.samples)
            suite.check_artifact_compatibility(record['cases'], metadata['diagnostics_version'])
            print(json.dumps(dict(**record, artifact=metadata), indent=2))
            return 0
        if args.summarize:
            return summarize_existing(root, args.summarize)
        if args.inputs is None or args.output is None:
            parser.error('Collection requires --inputs and --output')
        with device_lock():
            return execute(args, root, artifact, metadata)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
        print('Collector stopped: '+str(error), file=sys.stderr)
        raise SystemExit(1)
