#!/usr/bin/env python3
"""The actual PLOAM ranging caller cannot write hardware or publish O5."""
from pathlib import Path
import unittest
from test_pon_lifecycle import function
from pon_test_utils import run_c

class RangingTests(unittest.TestCase):
    def test_destinations_state_full_width_and_deferred_ack(self):
        fixture = Path(__file__).with_name('pon_ranging_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */',
              function('gpon/gpon_ploam.c', 'ploam_recv_ranging_time')))
