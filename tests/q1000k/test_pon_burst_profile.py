#!/usr/bin/env python3
"""PLOAM profile parsing hands complete immutable inputs to the install owner."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
from test_pon_lifecycle import function
from test_pon_phy_lifecycle import BSP

class BurstProfileTests(unittest.TestCase):
    def test_destinations_fields_and_error_forwarding(self):
        header = (BSP/'include/q1000k_phy_api.h').read_text()
        profile = re.search(r'struct q1000k_pon_profile \{.*?\n\};', header, re.S).group(0)
        fixture = Path(__file__).with_name('pon_burst_profile_fixture.c').read_text()
        run_c(fixture.replace('/* PROFILE TYPE */', profile).replace('/* PRODUCTION */',
              function('gpon/gpon_ploam.c', 'ploam_recv_profile')))
