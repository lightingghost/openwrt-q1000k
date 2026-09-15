#!/usr/bin/env python3
"""Check recovery boundaries in saved observations, without device access."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('matrix_report', ROOT / 'scripts/q1000k/bench-matrix-report.py')
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


class TransitionTests(unittest.TestCase):
    def test_no_attempt_and_first_observed_transition(self):
        before = dict(sampled_ms=1000, poll_calls=9, reacquire_attempts=0,
                      receiver={'clock': 0x70, 'reset': 3})
        first = dict(sampled_ms=2100, poll_calls=10, reacquire_attempts=1,
                     receiver={'clock': 0, 'reset': 3})
        last = dict(sampled_ms=3200, poll_calls=11, reacquire_attempts=1,
                    receiver={'clock': 1, 'reset': 3})
        self.assertIsNone(REPORT.transition([before]))
        result = REPORT.transition([before, first, last])
        self.assertEqual(result['first_observed_sample'], 2)
        self.assertEqual(result['observed_after_ms'], 1100)
        self.assertEqual(result['post_attempt_observation_ms'], 1100)
        self.assertEqual(result['changed_phy_words'], {
            'clock': {'last_before': '0x00000070', 'first_after': '0x00000000', 'final': '0x00000001'}})

    def test_rejects_missing_before_sample_or_reversed_time(self):
        sample = dict(sampled_ms=1000, poll_calls=10, reacquire_attempts=1, receiver={'clock': 0})
        with self.assertRaises(ValueError):
            REPORT.transition([sample])
        with self.assertRaises(ValueError):
            REPORT.transition([dict(sample, sampled_ms=1100, reacquire_attempts=0), sample])


if __name__ == '__main__':
    unittest.main()
