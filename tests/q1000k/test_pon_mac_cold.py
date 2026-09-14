#!/usr/bin/env python3
"""Cold discovery baseline, hardware field positions and fault containment."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
from test_pon_auth import MAC

class MacColdTests(unittest.TestCase):
    def test_cold_identity_masks_ownership_and_every_write_failure(self):
        source = (MAC/'inc/common/q1000k_mac_cold.h').read_text() + (MAC/'src/q1000k_mac_cold.c').read_text()
        source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
        fixture = Path(__file__).with_name('pon_mac_cold_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', source))
