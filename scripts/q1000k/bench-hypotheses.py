#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Evaluate receiver hypotheses from a saved capture; never access hardware.

These observations narrow a fault domain. They do not prove optical calibration,
CDR lock, registration or subscriber service.
"""
import argparse
import importlib.util
import json
from pathlib import Path

SPEC = importlib.util.spec_from_file_location('receiver', Path(__file__).with_name('bench-receiver-report.py'))
RECEIVER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RECEIVER)


def evaluate(report):
    if report['receiver_version'] != 5:
        raise ValueError('Hypothesis evaluation requires receiver schema 5')
    regs = {k: [int(v, 16) for v in values] for k, values in report['phy_words'].items()}
    pcs = report['pcs_counters']
    power = report['optical']
    enabled = all(v & (1 << 16) for v in regs['rx_control'])
    reset_released = all(v & 3 == 3 for v in regs['pcs_reset'])
    clear_inactive = all(not v & (1 << 16) for v in regs['pcs_debug_control'])
    phases = report.get('pcs_counter_phases') or {'all': pcs}
    activity = {k: any(p[k]['delta_mod32'] for p in phases.values()) for k in pcs}
    framing_activity = any(activity[k] for k in ('psync_mismatch', 'sfc_hec_error', 'pon_id_hec_error'))
    codeword_activity = any(activity[k] for k in ('cw_start', 'cw_end'))
    mac_activity = any(activity[k] for k in ('sof_to_mac', 'eof_to_mac'))
    light = report['rx_states']['controller_los'] == [False] and report['rx_states']['phy_los'] == [False]
    hypotheses = [
        dict(id='optical-power', hypothesis='Insufficient or unstable received optical power',
             observation=power, assessment='Measurement available; compare with the gateway under the same connection' if power['available_samples'] else 'No usable measurement; light indication alone cannot settle this',
             test='Compare connected power with the gateway reading; repeat disconnected and reconnected captures on this image'),
        dict(id='false-light', hypothesis='LOS polarity, forced signal detect or stale controller state',
             observation=dict(both_los_clear=light, sfp_status=report['phy_words']['sfp_status'], sfp_polarity=report['phy_words']['sfp_polarity']),
             assessment='Requires the disconnected control capture; agreement alone does not establish valid data',
             test='Both LOS sources must change on disconnection and recover on reconnection; correlate power'),
        dict(id='receiver-gain', hypothesis='RX frontend gain or equalization differs from the board requirement',
             observation=dict(gain=report['phy_words']['rx_frontend_gain'], equalizer=report['phy_words']['rx_equalizer'], experiment=report['gain_restore_requested']),
             assessment='Compare baseline and isolated gain trial; power alone does not prove electrical signal quality',
             test='One checked OEM gain trial, with original fields restored at shutdown'),
        dict(id='clock-rate-reset', hypothesis='CDR/PLL, rate, clock divider or reset sequencing prevents decoding',
             observation={k: report['phy_words'][k] for k in ('digital_status', 'rx_frequency', 'pll_status', 'rx_clock_divider', 'rx_cdr_ratio', 'rx_rate_control', 'rx_osr_control')},
             assessment='PCS reset released throughout' if reset_released else 'PCS reset was not consistently released',
             test='Compare rate/clock readbacks with the XGS-PON reference and existing recovery modes; forced lock flags do not prove CDR lock'),
        dict(id='receive-path', hypothesis='Wrong RX input, bus packing, polarity or controller analog state',
             observation={k: report['phy_words'][k] for k in ('rx_input_control', 'rx_bus_width', 'serdes_control', 'signal_control', 'rx_frontend_power', 'rx_control')},
             assessment='Configuration captured; physical differential polarity is not identified by these words',
             test='Audit against OEM/reference routing; no unsupported polarity or input-mux write is performed'),
        dict(id='pcs-framing', hypothesis='PCS disabled, counter clear held, or framing/descrambler configuration wrong',
             observation=dict(rx_enabled=enabled, pcs_reset_released=reset_released, counter_clear_inactive=clear_inactive, framing_error_activity=framing_activity),
             assessment='Simple enable/reset/held-clear explanations are disfavored' if enabled and reset_released and clear_inactive else 'A basic PCS control condition needs investigation',
             test='Inspect PCS controls and PSync/HEC counters together; errors without frames localize failure before valid frame delivery'),
        dict(id='measurement-boundary', hypothesis='The frame counter hides earlier reception or a later MAC boundary blocks delivery',
             observation=dict(codeword_activity=codeword_activity, mac_boundary_activity=mac_activity, counters=pcs),
             assessment='Earlier receive activity exists' if codeword_activity or framing_activity or mac_activity else 'No activity observed at any sampled PCS boundary; analog failure and incorrect counter configuration remain possible',
             test='Compare codeword start/end, frame-to-PHY, SOF/EOF-to-MAC and HEC counters; zero authorized subscriber traffic is expected in this bench'),
        dict(id='line-type', hypothesis='External signal is not the expected XGS-PON downstream',
             observation='The reported 3FE46901AC part was identified as XGS-PON in the recorded Nokia guide',
             assessment='Lower priority; neither LOS nor average power proves wavelength or modulation',
             test='Confirm the gateway optical mode/part and connection; wavelength or rate verification requires external equipment if ambiguity remains'),
        dict(id='analog-health', hypothesis='Controller firmware/calibration or optical/electrical hardware is unhealthy',
             observation=report['controller_words'], assessment='Firmware readback and MCU-enable checks do not prove analog operation',
             test='Compare firmware/status, power and LOS across controlled reconnects; do not rerun calibration-write commands'),
    ]
    return dict(schema_version=1, device_access=False, optical_service_verified=False,
                capture=report['capture'], hypotheses=hypotheses,
                excluded_next_step='OLT identity, OMCI provisioning, DHCP and VLAN changes cannot repair the current pre-registration PHY evidence; keep TX inhibited')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(evaluate(RECEIVER.summarize(args.capture)), indent=2))
    except (OSError, KeyError, TypeError, ValueError) as error:
        parser.exit(1, f'Cannot evaluate receiver hypotheses: {error}\n')


if __name__ == '__main__':
    main()
