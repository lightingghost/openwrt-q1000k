#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Summarize complete RX diagnostic captures, retaining a failed bench result.

Reads saved logs only. This is not the passing-bench acceptance report.
"""
import argparse
import json
import importlib.util
from pathlib import Path

_spec = importlib.util.spec_from_file_location('bench_report', Path(__file__).with_name('bench-report.py'))
_report = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_report)

PHY_WORDS = ('rx_control', 'pcs_reset', 'pma_reset', 'clock_control',
             'cdr_control', 'rx_frequency', 'pll_status', 'tdc_control',
             'rx_analog0', 'rx_analog1', 'rx_analog2',
             'rx_sequence_force', 'rx_sequence_disable')
CONTROLLER_WORDS = ('mcu_a0', 'mcu_a2', 'apd_control', 'ocp_control',
                    'firmware_status', 'los_control', 'system_status')
EXTENDED_PHY_WORDS = ('rx_sequence_force0', 'rx_sequence_disable0',
                      'rx_lock_force', 'rx_lock_disable', 'rx_oscal_control',
                      'rx_reset0', 'rx_reset1', 'pll_power', 'pll_filter',
                      'pll_pcw1', 'pll_pcw2')
PLL_PHY_WORDS = ('pll_force', 'pll_measure', 'pll_kband', 'pll_outputs')
PATH_PHY_WORDS = ('sfp_status', 'sfp_polarity', 'digital_status', 'pcs_debug_control', 'serdes_control', 'rx_clock_divider', 'rx_bus_width', 'rx_input_control', 'rx_cdr_ratio', 'rx_rate_control', 'rx_osr_control', 'signal_control', 'rx_equalizer', 'rx_frontend_power')
PCS_COUNTERS = ('cw_start', 'cw_end', 'sof_to_mac', 'eof_to_mac', 'psync_mismatch', 'sfc_hec_error', 'pon_id_hec_error')
COUNTERS = ('sampled_ms', 'frames', 'lof', 'fec_total', 'fec_corrected',
            'fec_uncorrected', 'irq_calls', 'poll_calls')
CONTROLLER_V2_WORDS = ('rx_output_control', 'rx_output_shape', 'ocp_status',
                       'temperature_raw', 'supply_raw', 'apd_voltage_raw',
                       'rssi_adc', 'rssi_current_raw',
                       'rx_output_original_control', 'rx_output_original_shape')
OUTPUT_PROFILES = {'unchanged': (0, None, None),
                   '400-flat': (1, 0x40, 0x14001400),
                   '600-flat': (2, 0x40, 0x1e001e00),
                   '600-boost': (3, 0, 0x36083208)}


def controller_diagnostics(rows, record):
    versions = {r.get('controller_version', 1) for r in rows}
    if versions == {1}:
        if record.get('oem_md32') or record.get('rx_output', 'unchanged') != 'unchanged':
            raise ValueError('Controller experiment requires version 2 observations')
        return None
    if versions not in ({2}, {3}) or any(type(r.get('controller_version')) is not int for r in rows):
        raise ValueError('Inconsistent controller diagnostics version')
    version = next(iter(versions))
    post = record.get('probe') in ('oem-post-init', 'oem-post-cal')
    if post and version < 3:
        raise ValueError('OEM post-init requires controller diagnostics version 3')
    oem = record.get('oem_md32', False)
    output = record.get('rx_output', 'unchanged')
    if type(oem) is not bool or output not in OUTPUT_PROFILES:
        raise ValueError('Invalid controller experiment selection')
    mode, control, shape = OUTPUT_PROFILES[output]
    raw = words(rows, CONTROLLER_V2_WORDS)
    for r in rows:
        post_mask = 0
        if version == 3:
            saved, original = r.get('oem_post_saved'), r.get('oem_post_original_control')
            if type(saved) is not bool or type(original) is not int or not 0 <= original <= 0xffffffff:
                raise ValueError('Invalid OEM post-init observations')
            if saved:
                if not post or r['rx_output_control'] != (original | 0x100):
                    raise ValueError('OEM post-init changed unexpected fields or lost its bit')
                post_mask = 0x100
        if (r.get('bench_md32_a0') is not oem or type(r.get('bench_rx_output')) is not int or
                r['bench_rx_output'] != mode or r.get('rx_output_saved') is not bool(mode) or
                r.get('rx_output_mask_control') != 0x40 or
                r.get('rx_output_mask_shape') != 0x3f1f3f08):
            raise ValueError('Controller experiment observations do not match requested profile')
        if mode and (r['rx_output_control'] & 0x40 != control or
                     r['rx_output_shape'] & 0x3f1f3f08 != shape):
            raise ValueError('Electrical output fields did not retain the selected profile')
        if mode and ((r['rx_output_control'] ^ r['rx_output_original_control']) & ~(0x40 | post_mask) or
                     (r['rx_output_shape'] ^ r['rx_output_original_shape']) & ~0x3f1f3f08):
            raise ValueError('Electrical output changed unrelated fields')
        if any(r[k] > 0xffff for k in ('temperature_raw', 'supply_raw', 'apd_voltage_raw',
                                     'rssi_adc', 'rssi_current_raw')):
            raise ValueError('Invalid controller analog sample width')
    return dict(controller_version=version, oem_md32=oem, rx_output=output, raw_words=raw,
                oem_post_saved=sorted({r['oem_post_saved'] for r in rows}) if version == 3 else None,
                apd_voltage_v=sorted({r['apd_voltage_raw']/8 for r in rows}),
                rssi_current_ua=sorted({r['rssi_current_raw'] >> 5 for r in rows}),
                temperature_c=sorted({(r['temperature_raw']-(65536 if r['temperature_raw'] & 0x8000 else 0))/256 for r in rows}),
                supply_v=sorted({r['supply_raw']/10000 for r in rows}),
                ocp_detected=sorted({bool(r['ocp_status'] & 256) for r in rows}),
                limits=['Analog telemetry is reported by the controller and does not prove high-speed electrical data.',
                        'Electrical profiles come from EN7572 family source; they are not verified Q1000K defaults.'])


def analog_fields(rows):
    """Decode only public AN7581 masks; these are not data/lock acceptance.

    en7581_pma.c XPON_readout_EO supplies DAC/offset masks; en7581_reg.h
    ADD_RO_RX2ANA_3 supplies FIFO full/empty nibbles and PI calibration.
    No eye scan, latch, selector or clear is executed by this decoder.
    """
    fields = {
        'dac_eye': ('rx_analog0', 0, 0x7f),
        'dac_d0': ('rx_analog0', 8, 0x7f),
        'dac_d1': ('rx_analog0', 16, 0x7f),
        'dac_e0': ('rx_analog0', 24, 0x7f),
        'dac_e1': ('rx_analog1', 0, 0x7f),
        'frontend_offset': ('rx_analog1', 8, 0x3f),
        'fifo_full_count_raw': ('rx_analog2', 16, 0x0f),
        'fifo_empty_count_raw': ('rx_analog2', 8, 0x0f),
        'pi_calibration_raw': ('rx_analog2', 0, 0x7f),
    }
    return {name: sorted({(r[word] >> shift) & mask for r in rows})
            for name, (word, shift, mask) in fields.items()}


def words(rows, names):
    for row in rows:
        for name in names:
            value = row.get(name)
            if type(value) is not int or not 0 <= value < 0xffffffff:
                raise ValueError('Invalid receiver word: ' + name)
    return {name: [f'0x{value:08x}' for value in sorted({x[name] for x in rows})]
            for name in names}


def summarize(capture):
    capture = capture.resolve(strict=True)
    record = json.loads((capture / 'checkpoint.json').read_text())
    if (record.get('schema_version') != 1 or record.get('action') != 'receive' or
            record.get('host') != '192.168.0.1' or
            record.get('fiber') not in ('connected', 'disconnected') or
            record.get('status') not in ('passed', 'failed')):
        raise ValueError('A completed receive capture is required')
    if record.get('postflight') != 'passed' or record.get('input_cleanup') != 'passed':
        raise ValueError('Capture cleanup has not been verified')
    objects = [json.loads(x) for x in (capture / 'attempt.log').read_text().splitlines()
               if x.startswith('{')]
    rx = [x for x in objects if x.get('rx_bench') is True]
    controller = [x for x in objects if x.get('receiver_status') is True]
    count = record.get('samples', 30)
    if type(count) is not int or count not in (30, 90, 180):
        raise ValueError('Invalid receive observation count')
    if len(rx) != count or len(controller) != count + 1:
        raise ValueError(f'Expected {count} RX and {count + 1} controller snapshots')
    versions = {x.get('receiver_version', 1) for x in rx}
    if versions not in ({1}, {2}, {3}, {4}, {5}) or any(type(x.get('receiver_version', 1)) is not int for x in rx):
        raise ValueError('Inconsistent receiver diagnostic version')
    names = PHY_WORDS + (EXTENDED_PHY_WORDS if versions != {1} else ())
    names += PLL_PHY_WORDS if versions in ({3}, {4}, {5}) else ()
    names += ('rx_frontend_gain',) if versions in ({4}, {5}) else ()
    names += PATH_PHY_WORDS if versions == {5} else ()
    if versions == {5}:
        for row in rx:
            for key in PCS_COUNTERS:
                value = row.get('pcs_counters', {}).get(key)
                if type(value) is not int or not 0 <= value <= 0xffffffff:
                    raise ValueError('Invalid PCS counter: ' + key)
    last = -1
    last_attempts = 0
    restore_gain = record.get('restore_gain', False)
    restore_pll = record.get('restore_pll', False)
    reacquire = record.get('reacquire_once', False)
    if type(restore_gain) is not bool or (restore_gain and not reacquire):
        raise ValueError('Invalid receiver gain restoration request')
    if type(restore_pll) is not bool or (restore_pll and not reacquire):
        raise ValueError('Invalid PLL restoration request')
    if type(reacquire) is not bool or (reacquire and record['fiber'] != 'connected'):
        raise ValueError('Invalid receive reacquisition request')
    for item in rx:
        for key, value in {'tx_inhibited': True, 'tx_enabled': False,
                           'registration_enabled': False}.items():
            if item.get(key) is not value:
                raise ValueError('Receive guard failed: ' + key)
        if type(item.get('mac_irq_mask')) is not int or item['mac_irq_mask'] != 0:
            raise ValueError('MAC interrupts were not masked')
        for key in COUNTERS + ('sync_status',):
            if type(item.get(key)) is not int or item[key] < 0:
                raise ValueError('Invalid receive counter: ' + key)
        for key in ('controller_los', 'phy_los', 'synced'):
            if type(item.get(key)) is not bool:
                raise ValueError('Invalid receive flag: ' + key)
        if item['sampled_ms'] <= last:
            raise ValueError('Stale receive sample')
        last = item['sampled_ms']
        if restore_gain or 'gain_restore_enabled' in item or item.get('receiver_version', 1) >= 4:
            if item.get('gain_restore_enabled') is not restore_gain:
                raise ValueError('Receiver gain restoration guard failed')
        if restore_pll or 'pll_restore_enabled' in item or item.get('receiver_version', 1) >= 3:
            if item.get('pll_restore_enabled') is not restore_pll:
                raise ValueError('PLL restoration guard failed')
        if reacquire or 'reacquire_enabled' in item or 'reacquire_attempts' in item:
            attempts = item.get('reacquire_attempts')
            if (item.get('reacquire_enabled') is not reacquire or type(attempts) is not int or
                    not last_attempts <= attempts <= (6 if record.get('probe') == 'oem-reset-repeat' and reacquire else int(reacquire)) or
                    (attempts and item['poll_calls'] < 10)):
                raise ValueError('Receive reacquisition guard failed')
            last_attempts = attempts
    return {
        'schema_version': 1, 'capture': str(capture), 'revision': record['revision'],
        'bench_result': record['status'], 'bench_error': record.get('error'),
        'report_kind': 'diagnostic observations, not service acceptance',
        'optical_service_verified': False, 'fiber': record['fiber'],
        'postflight': record['postflight'], 'input_cleanup': record['input_cleanup'],
        'rx_samples': len(rx), 'controller_samples': len(controller),
        'tx_inhibited': True, 'tx_enabled': False, 'registration_enabled': False,
        'mac_irq_mask': 0,
        'receiver_version': next(iter(versions)),
        'reacquire_requested': reacquire, 'reacquire_attempts': last_attempts,
        'pll_restore_requested': restore_pll, 'gain_restore_requested': restore_gain,
        'optical': _report.optical_summary(rx),
        'pcs_counters': {key: {'first': rx[0]['pcs_counters'][key],
                              'last': rx[-1]['pcs_counters'][key],
                              'delta_mod32': None if reacquire else (rx[-1]['pcs_counters'][key] - rx[0]['pcs_counters'][key]) & 0xffffffff}
                         for key in PCS_COUNTERS} if versions == {5} else None,
        'pcs_counter_phases': {str(n): {key: {
            'samples': len(items),
            'delta_mod32': (items[-1]['pcs_counters'][key]-items[0]['pcs_counters'][key]) & 0xffffffff if len(items)>1 else None
            } for key in PCS_COUNTERS} for n in range(last_attempts+1)
            for items in [[x for x in rx if x.get('reacquire_attempts',0)==n]]} if versions == {5} else None,
        'phy_words_by_attempt': {str(n): words([x['receiver'] for x in rx if x.get('reacquire_attempts',0)==n], names) for n in range(last_attempts+1)},
        'phy_words_before_reacquire': words([x['receiver'] for x in rx
                                            if x.get('reacquire_attempts', 0) == 0], names),
        'phy_words_after_reacquire': words([x['receiver'] for x in rx
                                           if x.get('reacquire_attempts', 0) > 0], names),
        'rx_states': {k: sorted({x[k] for x in rx}) for k in
                      ('controller_los', 'phy_los', 'synced', 'sync_status')},
        'counters': {k: {'first': rx[0][k], 'last': rx[-1][k]} for k in COUNTERS},
        'phy_words': words([x['receiver'] for x in rx], names),
        'analog_fields': analog_fields([x['receiver'] for x in rx]),
        'analog_limits': ['DAC/offset codes are passive calibration snapshots and may be stale without the reference latch operation.',
                          'No eye scan or latch was performed; these codes are not a measured eye opening.',
                          'FIFO nibbles may latch, wrap or saturate; changes are not counted as received frames.',
                          'No independent clock/data validity follows from these decoded fields.'],
        'controller_words': words(controller, CONTROLLER_WORDS),
        'controller_diagnostics': controller_diagnostics(controller, record),
        'elapsed_seconds': round(record['finished'] - record['started'], 3),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    try:
        result = summarize(args.capture)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f'Cannot summarize receiver diagnostics: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
