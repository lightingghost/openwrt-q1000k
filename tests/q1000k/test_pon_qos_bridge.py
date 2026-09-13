#!/usr/bin/env python3
"""Check real XMCS/native QoS translation and unchanged outputs on failure."""
import unittest
from pathlib import Path
from test_pon_lifecycle import function
from pon_test_utils import run_c

class PonQosBridgeTests(unittest.TestCase):
    def test_scheduler_translation_and_errors(self):
        production = function('xmcs/xmcs_if.c', 'xmcs_set_channel_scheduler') + function(
            'xmcs/xmcs_if.c', 'xmcs_get_channel_scheduler')
        fixture = Path(__file__).with_name('pon_qos_bridge_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', production))

if __name__ == '__main__':
    unittest.main()
