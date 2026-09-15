#!/usr/bin/env python3
"""Reject mismatched controller experiments and preserve old capture schemas."""
import importlib.util
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT/'scripts/q1000k'/f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

REPORT = load('bench-receiver-report')
PROBE = load('bench-probe-report')


class AcquisitionReportTests(unittest.TestCase):
    def test_controller_profile_and_preservation(self):
        for name, (mode, control, shape) in REPORT.OUTPUT_PROFILES.items():
            row = dict.fromkeys(REPORT.CONTROLLER_V2_WORDS, 0)
            row.update(controller_version=2, bench_md32_a0=True, bench_rx_output=mode,
                       rx_output_saved=bool(mode), rx_output_mask_control=0x40,
                       rx_output_mask_shape=0x3f1f3f08, rx_output_control=control or 0,
                       rx_output_shape=shape or 0, temperature_raw=0xff00,
                       apd_voltage_raw=240, rssi_current_raw=320)
            checkpoint = dict(oem_md32=True, rx_output=name)
            result = REPORT.controller_diagnostics([row], checkpoint)
            self.assertEqual(result['temperature_c'], [-1])
            self.assertEqual(result['apd_voltage_v'], [30])
            self.assertEqual(result['rssi_current_ua'], [10])
            for key, value in [('bench_md32_a0', False), ('bench_rx_output', 9),
                               ('rx_output_saved', not bool(mode)),
                               ('rx_output_mask_shape', 0), ('rssi_adc', 65536)]:
                with self.subTest(profile=name, field=key), self.assertRaises(ValueError):
                    REPORT.controller_diagnostics([row | {key: value}], checkpoint)
            if mode:
                for key, bit in [('rx_output_control', 64), ('rx_output_shape', 256),
                                 ('rx_output_control', 1), ('rx_output_shape', 1)]:
                    with self.assertRaises(ValueError):
                        REPORT.controller_diagnostics([row | {key: row[key] ^ bit}], checkpoint)

    def test_old_controller_cannot_validate_new_trial(self):
        self.assertIsNone(REPORT.controller_diagnostics([{}], {}))
        for record in [dict(oem_md32=True), dict(rx_output='600-flat')]:
            with self.assertRaises(ValueError):
                REPORT.controller_diagnostics([{}], record)

    def test_probe_enum_and_historical_schema(self):
        header = (ROOT/'package/kernel/airoha-pon/src/bsp/include/q1000k_phy_api.h').read_text()
        enum = header.split('enum q1000k_rx_probe {', 1)[1].split('};', 1)[0]
        names = re.findall(r'Q1000K_RX_PROBE_(\w+)', enum)[1:-1]
        self.assertEqual(tuple(n.lower().replace('_', '-') for n in names), PROBE.PROBES)
        self.assertEqual(len(PROBE.FIELDS_BY_VERSION[1]), 32)
        self.assertEqual(len(PROBE.FIELDS_BY_VERSION[2]), 34)
        self.assertEqual(len(PROBE.FIELDS_BY_VERSION[3]), 45)


if __name__ == '__main__':
    unittest.main()
