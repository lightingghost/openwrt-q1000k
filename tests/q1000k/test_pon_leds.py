#!/usr/bin/env python3
"""Exercise real xPON LED decisions, blink cancellation and reference unwind."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
ROOT = Path(__file__).resolve().parents[2]

class LedTests(unittest.TestCase):
    def test_optical_registration_and_lifetime(self):
        code = (ROOT / 'package/kernel/q1000k-omci/src/net/xpon/leds.c').read_text()
        code = re.sub(r'^#include[^\n]*\n', '', code, flags=re.M)
        fixture = Path(__file__).with_name('pon_leds_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', code))
