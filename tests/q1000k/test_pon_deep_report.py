#!/usr/bin/env python3
"""Fresh eye evidence, OEM post-init ownership and old-image rejection."""
import unittest
from test_pon_acquisition_report import PROBE, REPORT
from test_pon_bench_suite import SUITE


class DeepReportTests(unittest.TestCase):
    def line(self, done='01010000', ready='01010000'):
        return ('[123.45] q1000k: RX eye fresh=1 pi=00100000 done='+done+
                ' ready='+ready+' horizontal=01d00220 vertical=00006c14 '
                'dac0=12345678 dac1=00000009\n')

    def test_fresh_completion_and_signed_endpoints(self):
        result = PROBE.eye_observation(self.line(), 'oem-eye-4', 1)
        self.assertTrue(result['completed'])
        self.assertEqual(result['horizontal_ticks'], 80)
        self.assertEqual(result['vertical_ticks'], 40)
        self.assertEqual(result['endpoints']['bottom'], -20)
        self.assertFalse(result['data_lock_proven'])
        self.assertFalse(result['optical_ber_measured'])

    def test_missing_completion_remains_inconclusive(self):
        for done, ready in (('00000000', '01010000'), ('01010000', '00010000')):
            result = PROBE.eye_observation(self.line(done, ready), 'eye-current', 1)
            self.assertFalse(result['completed'])
            self.assertIsNone(result['horizontal_ticks'])
            self.assertIsNone(result['vertical_ticks'])
            self.assertIn('horizontal', result['raw'])

    def test_missing_duplicate_stale_or_unexpected_eye_is_rejected(self):
        for text, probe, attempt in (('', 'eye-current', 1), (self.line()*2, 'eye-current', 1),
                                      (self.line().replace('fresh=1', 'fresh=0'), 'eye-current', 1),
                                      (self.line(), 'checker', 1), (self.line(), 'eye-current', 0)):
            with self.subTest(probe=probe, text=text), self.assertRaises(ValueError):
                PROBE.eye_observation(text, probe, attempt)
        self.assertIsNone(PROBE.eye_observation('', 'eye-current', 0))

    def test_old_image_rejects_every_new_combination(self):
        for case in SUITE.plan(90)['cases']:
            if case['probe'] not in PROBE.PROBES[22:]:
                continue
            with self.subTest(case=case['name']), self.assertRaisesRegex(ValueError, 'schema 4'):
                SUITE.check_artifact_compatibility([case], 3)
            SUITE.check_artifact_compatibility([case], 4)
        self.assertEqual(SUITE.CONNECTED_PROBES[:2], ('oem-post-init', 'oem-post-cal'))

    def test_post_init_coexists_with_output_profile_without_relaxing_other_bits(self):
        for name, (mode, control, shape) in REPORT.OUTPUT_PROFILES.items():
            row = dict.fromkeys(REPORT.CONTROLLER_V2_WORDS, 0)
            row.update(controller_version=3, bench_md32_a0=False, bench_rx_output=mode,
                       rx_output_saved=bool(mode), rx_output_mask_control=64,
                       rx_output_mask_shape=0x3f1f3f08, rx_output_shape=shape or 0,
                       rx_output_control=(control or 0) | 0x100, oem_post_saved=True,
                       oem_post_original_control=control or 0)
            record = dict(rx_output=name, probe='oem-post-cal')
            self.assertEqual(REPORT.controller_diagnostics([row], record)['oem_post_saved'], [True])
            for change in (dict(rx_output_control=row['rx_output_control'] ^ 1),
                           dict(rx_output_control=row['rx_output_control'] ^ 0x100),
                           dict(oem_post_saved=1), dict(controller_version=2)):
                with self.subTest(name=name, change=change), self.assertRaises(ValueError):
                    REPORT.controller_diagnostics([row | change], record)
            with self.assertRaises(ValueError):
                REPORT.controller_diagnostics([row], dict(rx_output=name))

    def test_legacy_schemas_are_preserved(self):
        self.assertEqual([len(PROBE.FIELDS_BY_VERSION[v]) for v in (1, 2, 3, 4)], [32, 34, 45, 56])


if __name__ == '__main__':
    unittest.main()
